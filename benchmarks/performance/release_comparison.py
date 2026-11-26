#!/usr/bin/env python3
"""Build the same Bolt sources with two SDKs and compare verified process times."""
import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import json
from pathlib import Path
import random
import subprocess
import tempfile
from types import SimpleNamespace

from buffer_comparison import balanced_orders
from compare import describe, executable_path, measure, sha256_file
from suite import CASES, build, cpu_info, paired_median_interval


DEFAULT_CASES = ("list-edit", "list-search", "dictionary-string", "utf8-index",
                 "utf8-roundtrip", "buffer-truncate", "struct-buffer", "struct-zero", "queue-worklist")


def checked_time(exe, name, timeout):
    result = measure(exe, [], timeout)
    if result["stdout"] != CASES[name]:
        raise ValueError(f"{name}: expected {CASES[name]!r}, got {result['stdout']!r}")
    return result["time_ns"]


def run_pair(name, programs, args):
    durations = {label: [] for label in programs}
    rounds = []
    orders = balanced_orders(random.Random(args.seed), list(programs))
    for iteration in range(args.warmup + args.runs):
        order = next(orders)
        warmup = iteration < args.warmup
        times = {}
        for label in order:
            elapsed = checked_time(programs[label], name, args.timeout)
            times[label] = elapsed / 1e6
            if not warmup:
                durations[label].append(elapsed)
        rounds.append({"warmup": warmup, "order": order, "elapsed_ms": times})
    report = {"expected_stdout": CASES[name], "rounds": rounds}
    for label, values in durations.items():
        report[label] = describe(values)
    if args.runs >= 15:
        report["after_over_before_ratio_ci95"] = paired_median_interval(
            durations["before"], durations["after"], args.seed)
    return report


def run_suite_rounds(programs, args, report):
    rng = random.Random(args.seed)
    orders = {case: balanced_orders(random.Random(rng.getrandbits(64)), ["before", "after"])
              for case in programs}
    samples = {case: {"before": [], "after": []} for case in programs}
    totals = {"before": [], "after": []}
    report["suite_rounds"] = []
    for iteration in range(args.warmup + args.runs):
        warmup = iteration < args.warmup
        sequence = list(programs)
        rng.shuffle(sequence)
        elapsed = {"before": 0, "after": 0}
        pairs = []
        for case in sequence:
            pair = next(orders[case])
            pair_ns = []
            for label in pair:
                ns = checked_time(programs[case][label], case, args.timeout)
                pair_ns.append(ns)
                elapsed[label] += ns
                if not warmup:
                    samples[case][label].append(ns)
            pairs.append({"case": case, "order": pair, "ns": pair_ns})
        report["suite_rounds"].append({"warmup": warmup, "pairs": pairs, "total_ns": elapsed})
        if not warmup:
            for label in totals:
                totals[label].append(elapsed[label])
        print(f"Round {iteration+1}: {elapsed['before']/1e6:.1f} -> {elapsed['after']/1e6:.1f} ms", flush=True)

    def summarize(values):
        result = {label: describe(times) for label, times in values.items()}
        result["after_over_before_ratio_ci95"] = paired_median_interval(values["before"], values["after"], args.seed)
        return result

    report["suite_total"] = summarize(totals)
    for case, values in samples.items():
        report["cases"][case] = {"expected_stdout": CASES[case], **summarize(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, help="Path to the earlier bolt executable")
    parser.add_argument("--after", required=True, help="Path to the newer bolt executable")
    parser.add_argument("--cases", nargs="+", choices=CASES, default=DEFAULT_CASES)
    parser.add_argument("--suite-rounds", action="store_true", help="pair all cases in each shuffled round and measure the suite total")
    parser.add_argument("--keep-builds", type=Path, help="keep suite-round binaries in a new directory for sampling")
    parser.add_argument("--reuse-builds", type=Path, help="repeat suite rounds on an existing before/after build tree")
    parser.add_argument("--build-report", type=Path, help="verify reused binaries against this earlier measurement")
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--runs", type=int, default=15)
    parser.add_argument("--seed", type=int, default=1604)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--json", required=True, type=Path)
    args = parser.parse_args()
    if args.keep_builds and not args.suite_rounds:
        parser.error("--keep-builds requires --suite-rounds")
    if bool(args.reuse_builds) != bool(args.build_report) or (args.reuse_builds and (args.keep_builds or not args.suite_rounds)):
        parser.error("--reuse-builds requires --build-report and --suite-rounds with no --keep-builds")
    if args.runs < 1 or args.warmup < 0 or args.timeout <= 0 or len(set(args.cases)) != len(args.cases):
        parser.error("Require unique cases, runs > 0, warmup >= 0 and timeout > 0")
    compilers = {label: executable_path(path) for label, path in (("before", args.before), ("after", args.after))}
    cfg = SimpleNamespace(timeout=args.timeout, cflag=[], boltflag=[], codegen_dir=None, debug_backend="auto")
    report = {"utc": datetime.now(timezone.utc).isoformat(), "method": "whole process wall time, paired balanced random order",
              "host": cpu_info(), "warmup": args.warmup, "runs": args.runs, "seed": args.seed,
              "toolchains": {}, "cases": {}}
    for label, compiler in compilers.items():
        version = subprocess.check_output([str(compiler), "--version"], text=True, timeout=args.timeout).strip()
        report["toolchains"][label] = {"compiler": str(compiler), "sha256": sha256_file(compiler),
                                       "version": version, "cases": {}}
        runtime = compiler.parent / "BoltAotRuntime.lib"
        if runtime.is_file():
            report["toolchains"][label]["aot_runtime_sha256"] = sha256_file(runtime)

    def compile_pair(name, root):
        pair = {}
        for label, compiler in compilers.items():
            exe, metadata = build(name, "release", None, compiler, root / label, cfg)
            report["toolchains"][label]["cases"][name] = metadata
            pair[label] = exe
        return pair

    if args.reuse_builds:
        prior = json.loads(args.build_report.read_text(encoding="utf8"))
        programs = {name: {} for name in args.cases}
        for label, toolchain in report["toolchains"].items():
            earlier = prior["toolchains"][label]
            if any(toolchain.get(key) != earlier.get(key) for key in ("sha256", "aot_runtime_sha256")):
                raise ValueError(f"{label} SDK differs from the original build")
            for name in args.cases:
                metadata = earlier["cases"][name]
                exe = (args.reuse_builds / label / name / "release" / metadata["command"][-1]).resolve()
                if sha256_file(exe) != metadata["executable_sha256"]:
                    raise ValueError(f"binary changed: {exe}")
                for filename, digest in metadata.get("runtime_sha256", {}).items():
                    if sha256_file(exe.parent / filename) != digest:
                        raise ValueError(f"runtime changed: {exe.parent / filename}")
                programs[name][label] = exe
                toolchain["cases"][name] = metadata
        run_suite_rounds(programs, args, report)
    elif args.suite_rounds:
        if args.keep_builds:
            args.keep_builds.mkdir(parents=True, exist_ok=False)
        with (nullcontext(args.keep_builds) if args.keep_builds else tempfile.TemporaryDirectory()) as temp:
            programs = {name: compile_pair(name, Path(temp).resolve()) for name in args.cases}
            run_suite_rounds(programs, args, report)
    else:
        for name in args.cases:
            with tempfile.TemporaryDirectory(prefix="bolt-release-comparison-") as temp:
                result = run_pair(name, compile_pair(name, Path(temp)), args)
                report["cases"][name] = result
                print(f"{name}: {result['before']['median_ms']:.3f} -> {result['after']['median_ms']:.3f} ms", flush=True)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
