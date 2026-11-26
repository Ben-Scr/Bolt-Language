#!/usr/bin/env python3
"""Compare fresh Bolt front-end checks of a small program and a generated generic class."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import tempfile

from compare import describe, executable_path, sha256_file
from suite import captured, cpu_info, paired_median_interval


def generic_source():
    members = [f"int Field{i};" for i in range(64)]
    updates = "\n".join(f"  total = total + Field{i};" for i in range(64))
    for i in range(64):
        members.append(f"int Method{i}(T value) {{\n  int total=Field{i};\n{updates}\n  return total;\n}}")
    return "class Accumulator<T> { public:\n" + "\n".join(members) + "\n}\nstatic class Program { public: void Main() {} }\n"


def measure_case(source, tools, args):
    samples = {label: [] for label in tools}
    rounds = []
    command = ["check", source.name, "--release", "--no-timings"]
    for index in range(args.warmup + args.runs):
        order = ("before", "after") if index % 2 == 0 else ("after", "before")
        elapsed = {}
        for label in order:
            elapsed[label], message = captured([str(tools[label]), *command], source.parent, args.timeout)
            if message != "OK: 1 source file(s) checked":
                raise RuntimeError(f"{source.name}: unexpected check output: {message!r}")
            if index >= args.warmup:
                samples[label].append(elapsed[label])
        rounds.append({"warmup": index < args.warmup, "order": order, "elapsed_ns": elapsed})
    summary = {label: describe(times) for label, times in samples.items()}
    if args.runs >= 15:
        summary["after_over_before_ci95"] = paired_median_interval(samples["before"], samples["after"], 1301)
    return {"command": command, "source_sha256": sha256_file(source), "source_bytes": source.stat().st_size,
            "summary": summary, "rounds": rounds}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True)
    parser.add_argument("--bolt", required=True)
    parser.add_argument("--runs", type=int, default=15)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0 or args.timeout <= 0:
        parser.error("invalid sample count, warm-up count, or timeout")
    tools = {"before": executable_path(args.before), "after": executable_path(args.bolt)}
    report = {"utc": datetime.now(timezone.utc).isoformat(), "host": cpu_info(),
              "metric": "wall time from compiler launch through successful Release check and process exit",
              "warmup_pairs": args.warmup, "measured_pairs": args.runs, "tools": {}, "cases": {}}
    with tempfile.TemporaryDirectory(prefix="bolt-compiler-check-") as directory:
        work = Path(directory)
        for label, tool in tools.items():
            report["tools"][label] = {"path": str(tool), "sha256": sha256_file(tool),
                                      "version": captured([str(tool), "version"], work, args.timeout)[1]}
        arithmetic = work / "arithmetic.bolt"
        shutil.copyfile(Path(__file__).parent / "cases" / arithmetic.name, arithmetic)
        generic = work / "Scope.bolt"
        generic.write_bytes(generic_source().encode("utf-8"))
        for source in (arithmetic, generic):
            result = measure_case(source, tools, args)
            report["cases"][source.name] = result
            print(source.name, *(f"{label} {result['summary'][label]['median_ms']} ms" for label in tools), flush=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
