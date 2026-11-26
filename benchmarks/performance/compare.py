#!/usr/bin/env python3
"""Compare two runnable binaries. Measures whole-process time, not kernel time.

Standard library only. Refuses comparisons with differing program output/status.
Works on Windows/Linux/macOS provided both binaries can run on the host.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone


def windows_affinity():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    proc = kernel.GetCurrentProcess()
    mask_type = ctypes.c_size_t
    kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(mask_type), ctypes.POINTER(mask_type)]
    kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, mask_type]
    original, system = mask_type(), mask_type()
    if not kernel.GetProcessAffinityMask(proc, ctypes.byref(original), ctypes.byref(system)):
        raise ctypes.WinError(ctypes.get_last_error())

    def set_mask(mask):
        if not kernel.SetProcessAffinityMask(proc, mask):
            raise ctypes.WinError(ctypes.get_last_error())

    return original.value, set_mask


def executable_path(spec: str) -> Path:
    candidate = Path(spec).expanduser()
    if candidate.is_file():
        return candidate.resolve()
    path = shutil.which(spec)
    if path:
        return Path(path).resolve()
    raise FileNotFoundError(f"Executable not found: {spec}")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def measure(executable: Path, arguments: list[str], timeout: float) -> dict:
    t0 = time.perf_counter_ns()
    p = subprocess.run(
        [str(executable), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )
    elapsed_ns = time.perf_counter_ns() - t0
    if p.returncode != 0:
        raise RuntimeError(
            f"{executable.name} exited with {p.returncode}; stderr={p.stderr[-2000:]!r}"
        )
    return {
        "time_ns": elapsed_ns,
        "stdout": p.stdout.rstrip(),
        "returncode": p.returncode,
    }


def describe(values: list[int]) -> dict:
    ordered = sorted(values)
    med = statistics.median(ordered)
    return {
        "n": len(ordered),
        "median_ms": round(med / 1e6, 4),
        "p95_ms": round(ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)] / 1e6, 4),
        "min_ms": round(ordered[0] / 1e6, 4),
        "max_ms": round(ordered[-1] / 1e6, 4),
        "raw_ms": [round(x / 1e6, 4) for x in values],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, help="Reference executable (e.g. C)")
    parser.add_argument("--candidate", required=True, help="Candidate executable (e.g. Bolt)")
    parser.add_argument("--baseline-arg", action="append", default=[], help="Repeated baseline argument")
    parser.add_argument("--candidate-arg", action="append", default=[], help="Repeated candidate argument")
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--runs", type=int, default=21)
    parser.add_argument("--timeout", type=float, default=120.0, help="Seconds per execution")
    parser.add_argument("--min-duration-ms", type=float, default=200.0)
    parser.add_argument("--name", default="unnamed")
    parser.add_argument("--json", type=Path, help="Save detailed result as JSON")
    args = parser.parse_args()
    if args.warmup < 0 or args.runs < 3 or args.timeout <= 0 or args.min_duration_ms < 0:
        parser.error("Require warmup >= 0, runs >= 3, timeout > 0, min-duration-ms >= 0")

    baseline = executable_path(args.baseline)
    candidate = executable_path(args.candidate)
    observed_output = None
    durations = {"baseline": [], "candidate": []}
    rng = random.Random(442109)

    for i in range(args.warmup + args.runs):
        order = ["baseline", "candidate"]
        rng.shuffle(order)
        for label in order:
            exe = baseline if label == "baseline" else candidate
            program_args = args.baseline_arg if label == "baseline" else args.candidate_arg
            result = measure(exe, program_args, args.timeout)
            if observed_output is None:
                observed_output = result["stdout"]
            elif result["stdout"] != observed_output:
                raise RuntimeError(
                    f"Output mismatch in {label} iteration {i + 1}. "
                    f"Expected {observed_output[:250]!r}, got {result['stdout'][:250]!r}. "
                    "Do not benchmark programs with different results."
                )
            if i >= args.warmup:
                durations[label].append(result["time_ns"])

    baseline_data = describe(durations["baseline"])
    candidate_data = describe(durations["candidate"])
    speedup = statistics.median(durations["baseline"]) / statistics.median(durations["candidate"])
    process_is_long_enough = (
        min(baseline_data["median_ms"], candidate_data["median_ms"]) >= args.min_duration_ms
    )
    details = {
        "case": args.name,
        "measured_at_utc": datetime.now(timezone.utc).isoformat(),
        "metric": "whole_process_wall_clock_including_startup",
        "note": "Not a compile-time or isolated-kernel benchmark. Matching stdout only checks outputs, not semantic equivalence.",
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python": platform.python_version(),
        },
        "warmup_runs": args.warmup,
        "runs": args.runs,
        "min_duration_ms": args.min_duration_ms,
        "valid_duration_gate": process_is_long_enough,
        "baseline": {
            "executable": str(baseline),
            "sha256": sha256_file(baseline),
            "arguments": args.baseline_arg,
            **baseline_data,
        },
        "candidate": {
            "executable": str(candidate),
            "sha256": sha256_file(candidate),
            "arguments": args.candidate_arg,
            **candidate_data,
        },
        "speedup_baseline_over_candidate": round(speedup, 5),
        "output": observed_output,
    }
    print(f"{args.name}: {args.runs} measured pairs ({args.warmup} warm-up pairs)")
    print(f"Baseline median={baseline_data['median_ms']:.3f} ms, p95={baseline_data['p95_ms']:.3f} ms")
    print(f"Candidate median={candidate_data['median_ms']:.3f} ms, p95={candidate_data['p95_ms']:.3f} ms")
    print(f"Candidate speedup = {speedup:.4f}x (baseline / candidate; >1 is faster)")
    if not process_is_long_enough:
        print("WARNING: short executions; startup and OS noise may dominate. Not a valid runtime claim.")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(details, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"Detailed result: {args.json}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (FileNotFoundError, RuntimeError, subprocess.TimeoutExpired) as ex:
        print(f"Benchmark failed: {ex}", file=sys.stderr)
        sys.exit(2)
