#!/usr/bin/env python3
"""Measure a short Buffer program and a matching empty-process control on Linux."""
import argparse
import json
import os
from datetime import datetime, timezone
from pathlib import Path
import random
import subprocess
import tempfile

from compare import describe, executable_path, measure, sha256_file
from suite import cpu_info
from buffer_comparison import check_loader

ROOT = Path(__file__).resolve().parent
EMPTY = {"c": "int main(void) { return 0; }\n",
         "bolt": "static class Program { public: int Main() { return 0; } }\n"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", required=True)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cpu", required=True, type=int)
    parser.add_argument("--runs", type=int, default=40)
    parser.add_argument("--seed", type=int, default=371685)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--gate", action="store_true", help="Fail if any Bolt median or p95 exceeds its C reference")
    args = parser.parse_args()
    if args.runs < 1 or args.cpu not in os.sched_getaffinity(0):
        parser.error("select an available CPU and a positive number of rounds")
    os.sched_setaffinity(0, {args.cpu})
    build_tools = {"c": executable_path(args.cc), "bolt": executable_path(args.bolt)}
    report = {"utc_start": datetime.now(timezone.utc).isoformat(), "host": cpu_info(),
              "seed": args.seed, "runs": args.runs, "warmup_rounds": 2, "builds": {},
              "samples": [], "summary_ms": {}}
    with tempfile.TemporaryDirectory(prefix="bolt-startup-") as temp:
        programs, expected = {}, {}
        for workload in ("empty", "buffer", "buffer-consume", "buffer-consume-chunked", "buffer-fused"):
            for language, tool in build_tools.items():
                if workload not in ("empty", "buffer") and language == "bolt":
                    continue
                label = workload + "-" + language
                folder = Path(temp) / label
                folder.mkdir()
                source = folder / ("main." + language)
                if workload == "empty":
                    source.write_text(EMPTY[language])
                else:
                    source.write_bytes((ROOT / "cases" / (workload + "." + language)).read_bytes())
                exe = folder / "program"
                if language == "c":
                    command = [str(tool), "-std=c11", "-O3", "-DNDEBUG", source.name, "-o", exe.name]
                else:
                    command = [str(tool), "compile", source.name, "--release", "--backend", "llvm",
                               "--require-native", "-o", exe.name]
                result = subprocess.run(command, cwd=folder, capture_output=True, text=True)
                if result.returncode:
                    raise RuntimeError(f"failed to build {label}: {result.stderr}")
                build = {"command": command, "source_sha256": sha256_file(source),
                         "tool_sha256": sha256_file(tool), "executable_sha256": sha256_file(exe)}
                if workload == "empty":
                    build["source"] = EMPTY[language]
                if language == "bolt":
                    build["runtime_linkage"] = check_loader(exe, tool)
                report["builds"][label] = build
                programs[label] = exe
                expected[label] = "" if workload == "empty" else "17179607040"
        rng = random.Random(args.seed)
        times = {k: [] for k in programs}
        for trial in range(-2, args.runs):
            order = list(programs)
            rng.shuffle(order)
            row = {}
            for k in order:
                sample = measure(programs[k], [], 30)
                if sample["stdout"] != expected[k]:
                    raise RuntimeError(f"wrong output for {k}: {sample}")
                row[k] = sample["time_ns"]
                if trial >= 0:
                    times[k].append(row[k])
            if trial >= 0:
                report["samples"].append({"order": order, "time_ns": row})
        report["summary_ms"] = {k: describe(v) for k, v in times.items()}
    report["gate"] = {}
    for c_case in ("empty", "buffer", "buffer-consume", "buffer-consume-chunked", "buffer-fused"):
        name = "empty-bolt" if c_case == "empty" else "buffer-bolt"
        bolt, c = report["summary_ms"][name], report["summary_ms"][c_case + "-c"]
        report["gate"][c_case] = {key: bolt[key] <= c[key] for key in ("median_ms", "p95_ms")}
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    print({k: (v["median_ms"], v["p95_ms"]) for k, v in report["summary_ms"].items()})
    if args.gate and not all(all(row.values()) for row in report["gate"].values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
