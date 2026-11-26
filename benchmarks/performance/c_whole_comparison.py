"""Time full suites of Bolt Release programs and their C references in matched rounds."""
import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile
from types import SimpleNamespace

from buffer_comparison import balanced_orders
from compare import describe, executable_path, sha256_file, windows_affinity
from release_comparison import checked_time
from suite import CASES, TUNED_REFERENCES, build, cpu_info, paired_median_interval


# One published alternative per workload, selected before timing. Reuse the
# regular C measurement when the suite has no distinct implementation for it.
CONTROLS = {TUNED_REFERENCES[k]: k for k in (
    "pointers-mask", "buffer-fused", "buffer-long-fused-unroll8", "indirect-array-unchecked",
    "memory-long-prefix", "list-search-simd-unroll4", "utf8-roundtrip-simd",
    "utf8-roundtrip-long-simd", "list-search-long-avx2", "dictionary-string-long-cached",
    "queue-worklist-long-ring", "queue-worklist-ring", "dictionary-string-cached",
)}


def prepare(args, manifest, root, sdk, cc, before_sdk=None):
    cfg = SimpleNamespace(timeout=args.timeout, cflag=args.cflag, boltflag=[],
                          codegen_dir=None, debug_backend="auto")
    programs, metadata = {}, {}
    for name, old in manifest["toolchains"]["after"]["cases"].items():
        if CASES[name] != manifest["cases"][name]["expected_stdout"]:
            raise ValueError(f"{name}: suite oracle changed")
        if args.bolt_programs:
            directory = args.bolt_programs.resolve() / name / "release"
            exe = directory / old["command"][-1]
            if sha256_file(exe) != old["executable_sha256"]:
                raise ValueError(f"{name}: Bolt executable differs from the earlier measurement")
            for filename, digest in old.get("runtime_sha256", {}).items():
                if sha256_file(directory / filename) != digest:
                    raise ValueError(f"{name}: runtime differs from the earlier measurement")
            bolt_meta = old
        else:
            exe, bolt_meta = build(name, "release", None, sdk, root / "bolt", cfg)
            if bolt_meta["source_sha256"] != old["source_sha256"]:
                raise ValueError(f"{name}: Bolt source differs from the earlier measurement")
        programs[name] = {"bolt": exe}
        metadata[name] = {"bolt": bolt_meta}
        if before_sdk:
            previous, baseline = build(name, "release", None, before_sdk, root / "before", cfg)
            if baseline["source_sha256"] != bolt_meta["source_sha256"]:
                raise ValueError(f"{name}: before and after sources differ")
            programs[name]["before"] = previous
            metadata[name]["before"] = baseline
        variants = {"c": name}
        if name in CONTROLS:
            variants["c_control"] = CONTROLS[name]
        for label, fixture in variants.items():
            exe, meta = build(fixture, "c", cc, None, root / label, cfg)
            programs[name][label] = exe
            metadata[name][label] = {"case": fixture, **meta}
        for exe in programs[name].values():
            checked_time(exe, name, args.timeout)
    return programs, metadata


def summarize(times, seed):
    result = {key: describe(values) for key, values in times.items()}
    for label in ("c", "c_control"):
        result["bolt_over_" + label + "_ci95"] = paired_median_interval(times[label], times["bolt"], seed)
    if "before" in times:
        result["after_over_before_ci95"] = paired_median_interval(times["before"], times["bolt"], seed)
        for label in ("c", "c_control"):
            result["before_over_" + label + "_ci95"] = paired_median_interval(times[label], times["before"], seed)
    return result


def run_series(programs, runs, warmup, seed, timeout):
    rng = random.Random(seed)
    orders = {name: balanced_orders(random.Random(rng.getrandbits(64)), list(bins))
              for name, bins in programs.items()}
    labels = ("bolt", "c", "c_control")
    if "before" in next(iter(programs.values())):
        labels = ("before", *labels)
    totals = {k: [] for k in labels}
    samples = {case: {k: [] for k in labels} for case in programs}
    rounds = []
    for i in range(warmup + runs):
        names = list(programs)
        rng.shuffle(names)
        total = dict.fromkeys(labels, 0)
        timings = []
        for name in names:
            sequence = next(orders[name])
            times = {k: checked_time(programs[name][k], name, timeout) for k in sequence}
            timings.append({"case": name, "order": sequence, "ns": times.copy()})
            times.setdefault("c_control", times["c"])
            for k in labels:
                total[k] += times[k]
                if i >= warmup:
                    samples[name][k].append(times[k])
        rounds.append({"warmup": i < warmup, "samples": timings, "total_ns": total})
        if i >= warmup:
            for k in labels:
                totals[k].append(total[k])
        print(f"round {i+1}/{warmup+runs}: " + ", ".join(f"{k} {t/1e9:.3f}s" for k,t in total.items()), flush=True)
    return {"seed": seed, "warmup": warmup, "runs": runs, "rounds": rounds,
            "suite": summarize(totals, seed),
            "cases": {name: summarize(times, seed) for name,times in samples.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", required=True)
    parser.add_argument("--before", help="build a second SDK on the same sources and time both alongside C")
    parser.add_argument("--keep-builds", type=Path, help="retain the measured executables and objects for profiling in a new directory")
    parser.add_argument("--cc", required=True)
    parser.add_argument("--manifest", type=Path, required=True, help="earlier verified Bolt release comparison")
    parser.add_argument("--bolt-programs", type=Path, help="reuse the exact after/ executables from that comparison")
    parser.add_argument("--cflag", action="append", default=[])
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--runs", type=int, default=15)
    parser.add_argument("--seed", type=int, default=4283)
    parser.add_argument("--pin-cpu", type=int, help="also run a second Windows series on this logical CPU")
    parser.add_argument("--pinned-runs", type=int, default=11)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    if min(args.runs, args.pinned_runs, args.timeout) < 1 or args.warmup < 0:
        parser.error("invalid repetition count or timeout")
    manifest = json.loads(args.manifest.read_text(encoding="utf8"))
    sdk, cc = executable_path(args.bolt), executable_path(args.cc)
    earlier = manifest["toolchains"]["after"]
    before_sdk = executable_path(args.before) if args.before else None
    verify_sdk = not before_sdk or args.bolt_programs
    if verify_sdk and sha256_file(sdk) != earlier["sha256"]:
        parser.error("Bolt SDK differs from the supplied manifest")
    if verify_sdk and "aot_runtime_sha256" in earlier and sha256_file(sdk.parent / "BoltAotRuntime.lib") != earlier["aot_runtime_sha256"]:
        parser.error("Bolt AOT runtime differs from the supplied manifest")
    mask, set_mask = windows_affinity() if os.name == "nt" else (None, None)
    if args.pin_cpu is not None and (mask is None or not 0 <= args.pin_cpu < mask.bit_length() or not (mask >> args.pin_cpu & 1)):
        parser.error("requested Windows logical CPU is unavailable")
    report = {"utc": datetime.now(timezone.utc).isoformat(), "host": cpu_info(), "normal_affinity_mask": mask,
              "manifest_sha256": sha256_file(args.manifest), "bolt_sha256": sha256_file(sdk),
              "aot_runtime_sha256": sha256_file(sdk.parent / "BoltAotRuntime.lib") if (sdk.parent / "BoltAotRuntime.lib").is_file() else None,
              "cc": str(cc), "cc_sha256": sha256_file(cc),
              "cc_version": subprocess.run([cc, "/Bv" if cc.stem.lower()=="cl" else "--version"],
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           text=True, errors="replace").stdout,
              "oracles": {n: d["expected_stdout"] for n,d in manifest["cases"].items()}, "series": {}}
    if before_sdk:
        report["before_sdk"] = str(before_sdk)
        report["before_sha256"] = sha256_file(before_sdk)
        runtime = before_sdk.parent / "BoltAotRuntime.lib"
        report["before_aot_runtime_sha256"] = sha256_file(runtime) if runtime.is_file() else None
    args.json.parent.mkdir(parents=True, exist_ok=True)
    if args.keep_builds:
        args.keep_builds.mkdir(parents=True, exist_ok=False)
    with (nullcontext(args.keep_builds.resolve()) if args.keep_builds else
          tempfile.TemporaryDirectory(prefix="bolt-c-whole-")) as work:
        programs, report["builds"] = prepare(args, manifest, Path(work), sdk, cc, before_sdk)
        print(f"Verified {len(programs)} workloads, {sum('c_control' in p for p in programs.values())} C controls", flush=True)
        report["series"]["normal"] = run_series(programs, args.runs, args.warmup, args.seed, args.timeout)
        args.json.write_text(json.dumps(report, indent=2)+"\n", encoding="utf8")
        if args.pin_cpu is not None:
            set_mask(1 << args.pin_cpu)
            try:
                report["pin_cpu"] = args.pin_cpu
                report["series"]["pinned"] = run_series(programs, args.pinned_runs, args.warmup, args.seed+1, args.timeout)
            finally:
                set_mask(mask)
            args.json.write_text(json.dumps(report, indent=2)+"\n", encoding="utf8")


if __name__ == "__main__":
    main()
