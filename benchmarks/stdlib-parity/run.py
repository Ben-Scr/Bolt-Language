#!/usr/bin/env python3
"""Compare public Bolt library calls with optimized C++20 programs on this host."""

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent
PERFORMANCE = ROOT.parent / "performance"
sys.path.insert(0, str(PERFORMANCE))
from compare import describe, executable_path, sha256_file, windows_affinity
from suite import CASES as EXISTING, cpu_info, paired_median_interval

EXPECTED = {
    "latin1-roundtrip": "7271056", "directory": "3504",
    "clock-monotonic": "6000000", "atomic-add-load": "32000004000000",
    "small-text": "22", "text-stream": "86016",
    "write-text": "147456", "read-text": "37761024",
    "file-stream": "780288", "memory-stream": "24969216",
    "integer-format": "105851386", "string-search": "911305112", "string-replace": "51792500",
    "math-log-exp": "4960172528", "math-sqrt": "120679561564", "math-trig": "153107669",
    "random-owned": "16000000", "random-shared": "16000000",
    "set-snapshot": str(512 * sum(i * 7 for i in range(4096) if i % 3)),
    **{name: EXISTING[name] for name in (
        "list-edit", "list-search", "list-materialized", "queue-worklist", "dictionary-string",
        "memory", "float-parse",
    )},
}
PRINT_SHA256 = "5207a7787f4ea7a7c6896b9e9f2e1ab5dc0d262f887b41945d69906e7cfe8e39"
SOURCES = {src.stem: src for src in sorted(ROOT.glob("*/*.cpp"))}
SOURCES["print-lines-flush"] = SOURCES["print-lines"]


@contextmanager
def run_cpu(cpu):
    if cpu is None:
        yield
        return
    if os.name == "nt":
        previous, apply = windows_affinity()
        if cpu < 0 or not previous & (1 << cpu):
            raise ValueError(f"CPU {cpu} is outside the allowed affinity")
        apply(1 << cpu)
    elif hasattr(os, "sched_setaffinity"):
        previous = os.sched_getaffinity(0)
        if cpu not in previous:
            raise ValueError(f"CPU {cpu} is outside the allowed affinity")
        def apply(mask):
            os.sched_setaffinity(0, mask)
        apply({cpu})
    else:
        raise ValueError("CPU pinning is unavailable on this host")
    try:
        yield
    finally:
        apply(previous)


def expected_file(case):
    if case in {"write-text", "read-text"}:
        return "text.bin", b"0123456789abcdef0123456789abcdef\xc3\x96\0\n" * 4096
    if case == "file-stream":
        block = bytes(range(128)) * 64
        return "stream.bin", b"".join(bytes([(31 + i) & 127]) + block[1:] for i in range(128))
    if case == "small-text":
        return "short.txt", "Grüße\0\né\0🙂\ndone\n".encode()
    if case == "text-stream":
        return "lines.txt", "Grüße\0😀\nsecond-é\n\n".encode() * 4096
    return None


def build(case, label, compiler, work):
    cpp = SOURCES[case]
    src = cpp if label == "cpp" else cpp.with_suffix(".bolt")
    if not src.is_file() and label != "cpp":
        src = PERFORMANCE / "cases" / (case + ".bolt")
    work.mkdir(parents=True)
    local = work / src.name
    shutil.copyfile(src, local)
    exe = work / ("program.exe" if os.name == "nt" else "program")
    if label != "cpp":
        args = ["compile", local.name, "--release", "--backend", "llvm", "--require-native",
                "--runtime-dir", str(compiler.parent), "-o", exe.name]
    elif compiler.stem.lower() in {"cl", "clang-cl"}:
        args = ["/nologo", "/std:c++20", "/O2", "/DNDEBUG", "/EHsc", "/utf-8",
                "/I" + str(src.parent), local.name, "/Fe:" + exe.name]
    else:
        args = ["-std=c++20", "-O3", "-DNDEBUG", "-I", str(src.parent), local.name, "-o", exe.name]
    if label == "cpp" and case == "print-lines-flush":
        args.insert(0, "/DFLUSH_EACH_LINE" if compiler.stem.lower() in {"cl", "clang-cl"} else "-DFLUSH_EACH_LINE")
    built = subprocess.run([compiler, *args], cwd=work, capture_output=True, timeout=180)
    if built.returncode:
        raise RuntimeError(f"{case}/{label}: {(built.stdout + built.stderr).decode(errors='replace')}")
    return exe, {"args": args, "source_sha256": sha256_file(src), "exe_sha256": sha256_file(exe),
                 "build_output": (built.stdout + built.stderr).decode(errors="replace").strip()}


def checked_run(exe, case, artifact, timeout):
    output = exe.parent / "stdout.bin"
    if artifact:
        (exe.parent / artifact[0]).unlink(missing_ok=True)
    with output.open("wb") as stream:
        start = time.perf_counter_ns()
        proc = subprocess.run([exe], cwd=exe.parent, stdout=stream, stderr=subprocess.PIPE, timeout=timeout)
        elapsed = time.perf_counter_ns() - start
    if proc.returncode:
        raise RuntimeError(f"{case}: exit {proc.returncode}: {proc.stderr.decode(errors='replace')}")
    if case in {"print-lines", "print-lines-flush"}:
        if output.stat().st_size != 2541668 or sha256_file(output) != PRINT_SHA256:
            raise ValueError(f"{case}: incorrect binary stdout from {exe}")
    else:
        text = output.read_bytes().replace(b"\r\n", b"\n")
        if text != (EXPECTED[case] + "\n").encode():
            raise ValueError(f"{case}: unexpected stdout from {exe}: {text[:180]!r}")
    if artifact and (exe.parent / artifact[0]).read_bytes() != artifact[1]:
        raise ValueError(f"{case}: incorrect file contents from {exe}")
    return elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", default="bolt")
    parser.add_argument("--before", help="also compile and run against the original Bolt SDK")
    parser.add_argument("--cxx", default="cl" if os.name == "nt" else "c++")
    parser.add_argument("--cases", nargs="+", choices=sorted(SOURCES), default=sorted(SOURCES))
    parser.add_argument("--runs", type=int, default=15)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--cpu", type=int, help="logical CPU for the measured programs")
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--fail-on-slower", action="store_true", help="fail on any Bolt median above C++")
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0 or args.timeout <= 0:
        parser.error("require runs >= 1, warmup >= 0, timeout > 0")
    compilers = {"cpp": executable_path(args.cxx), "bolt": executable_path(args.bolt)}
    if args.before:
        compilers["before"] = executable_path(args.before)
    tools = {}
    for label, compiler in compilers.items():
        version_arg = [] if compiler.stem.lower() == "cl" else ["--version"]
        version = subprocess.run([compiler, *version_arg], capture_output=True)
        tools[label] = {"path": str(compiler), "sha256": sha256_file(compiler),
                        "version": (version.stdout + version.stderr).decode(errors="replace").strip()}
        if label != "cpp":
            tools[label]["runtime_sha256"] = {
                name: sha256_file(compiler.parent / name)
                for name in ("BoltAotRuntime.lib", "BoltRuntime.dll", "BoltAotRuntime.o", "libBoltRuntime.so", "libBoltRuntime.dylib")
                if (compiler.parent / name).is_file()
            }
    report = {"metric": "whole-process wall time including process startup, stdout and close",
              "date_utc": datetime.now(timezone.utc).isoformat(), "host": cpu_info(), "tools": tools,
              "runs": args.runs, "warmup": args.warmup, "run_cpu": args.cpu, "cases": {}}
    args.json.parent.mkdir(parents=True, exist_ok=True)
    failures = False
    with tempfile.TemporaryDirectory(prefix="bolt-stdlib-parity-") as temp:
        for case in args.cases:
            record = report["cases"][case] = {}
            try:
                rng = random.Random("stdlib-" + case)
                labels = list(compilers)
                rng.shuffle(labels)
                builds = {label: build(case, label, compilers[label], Path(temp) / case / label)
                          for label in labels}
                samples = {label: [] for label in labels}
                artifact = expected_file(case)
                record["expected_stdout"] = ("sha256:" + PRINT_SHA256 if case.startswith("print-lines")
                                             else EXPECTED[case])
                record["order"] = []
                for index in range(-args.warmup, args.runs):
                    if index % len(labels) == 0:
                        rng.shuffle(labels)
                    offset = index % len(labels)
                    order = labels[offset:] + labels[:offset]
                    if index >= 0:
                        record["order"].append(order)
                    for label in order:
                        with run_cpu(args.cpu):
                            elapsed = checked_run(builds[label][0], case, artifact, args.timeout)
                        if index >= 0:
                            samples[label].append(elapsed)
                for label, values in samples.items():
                    record[label] = {**builds[label][1], **describe(values)}
                ratio = statistics.median(samples["bolt"]) / statistics.median(samples["cpp"])
                record["bolt_over_cpp"] = ratio
                if args.runs >= 15:
                    record["ratio_ci95"] = paired_median_interval(samples["cpp"], samples["bolt"], 1701)
                if args.before:
                    record["before_over_bolt"] = statistics.median(samples["before"]) / statistics.median(samples["bolt"])
                    if args.runs >= 15:
                        record["before_over_bolt_ci95"] = paired_median_interval(samples["bolt"], samples["before"], 1801)
                print(f"{case:18s} Bolt {record['bolt']['median_ms']:9.3f} ms   "
                      f"C++ {record['cpp']['median_ms']:9.3f} ms   Bolt/C++ {ratio:.3f}", flush=True)
                failures |= args.fail_on_slower and ratio > 1
            except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                record["error"] = str(error)
                print(f"FAIL {case}: {error}", file=sys.stderr, flush=True)
                failures = True
            args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return int(failures)


if __name__ == "__main__":
    sys.exit(main())
