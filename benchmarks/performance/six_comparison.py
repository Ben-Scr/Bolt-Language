"""Compare the six previously faster C references with two Bolt Release SDKs."""
import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import random
import subprocess
from tempfile import TemporaryDirectory
from types import SimpleNamespace

from buffer_comparison import balanced_orders
from c_whole_comparison import windows_affinity
from compare import describe, executable_path, sha256_file
from release_comparison import checked_time
from suite import CASES, build, cpu_info, paired_median_interval


REFERENCES = {
    "arithmetic": "arithmetic",
    "buffer-truncate-long": "buffer-truncate-long",
    "buffer-record-add": "buffer-record-add",
    "buffer-long": "buffer-long-fused-unroll8",
    "dictionary-string": "dictionary-string-cached",
    "utf8-roundtrip-long": "utf8-roundtrip-long-simd",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True)
    parser.add_argument("--after", required=True)
    parser.add_argument("--cc", required=True, help="Clang or GCC")
    parser.add_argument("--cases", nargs="+", choices=(*REFERENCES, "buffer-record-add-long"), default=list(REFERENCES))
    parser.add_argument("--pin-cpu", type=int, help="pin this Windows run to a logical processor")
    parser.add_argument("--runs", type=int, default=51)
    parser.add_argument("--warmup", type=int, default=4)
    parser.add_argument("--seed", type=int, default=215713)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--keep-builds", type=Path, help="keep binaries in a new directory")
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0 or args.timeout < 1:
        parser.error("require positive runs and timeout, and nonnegative warmup")
    if len(set(args.cases)) != len(args.cases):
        parser.error("duplicate benchmark case")
    if args.pin_cpu is not None:
        if os.name != "nt":
            parser.error("--pin-cpu requires Windows")
        mask, set_mask = windows_affinity()
        if not 0 <= args.pin_cpu < mask.bit_length() or not ((mask >> args.pin_cpu) & 1):
            parser.error("logical processor unavailable")
        set_mask(1 << args.pin_cpu)
    compilers = {key: executable_path(getattr(args, key)) for key in ("before", "after", "cc")}
    report = {"utc": datetime.now(timezone.utc).isoformat(), "host": cpu_info(),
              "pin_cpu": args.pin_cpu, "runs": args.runs, "warmup": args.warmup, "seed": args.seed,
              "method": "whole-process wall time including startup, verified stdout after every run",
              "compilers": {}, "cases": {}, "rounds": []}
    if report["host"]["platform"].startswith("Windows"):
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            report["host"]["cpu_model"] = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    for label, exe in compilers.items():
        info = {"path": str(exe), "sha256": sha256_file(exe),
                "version": subprocess.check_output([exe, "--version"], text=True, timeout=args.timeout).strip()}
        for library in ("BoltAotRuntime.lib", "BoltAotRuntime.o"):
            candidate = exe.parent / library
            if label != "cc" and candidate.exists():
                info[library] = sha256_file(candidate)
        report["compilers"][label] = info
    if args.keep_builds:
        args.keep_builds.mkdir(parents=True, exist_ok=False)
    with (nullcontext(args.keep_builds) if args.keep_builds else TemporaryDirectory()) as directory:
        root = Path(directory).resolve()
        programs = {}
        for name in args.cases:
            reference = REFERENCES.get(name, name)
            builds = {}
            programs[name] = {}
            for label in ("before", "after", "c"):
                flags = ["-march=native", "-ffp-contract=off"]
                if label == "c" and name == "utf8-roundtrip-long":
                    flags.append("-DBENCH_UTF8_KNOWN_EDIT=1")
                cfg = SimpleNamespace(timeout=args.timeout, cflag=flags, boltflag=[], codegen_dir=None, debug_backend="auto")
                program, builds[label] = build(
                    reference if label == "c" else name, "c" if label == "c" else "release",
                    compilers["cc"], compilers.get(label), root / label, cfg)
                programs[name][label] = program
            if builds["before"]["source_sha256"] != builds["after"]["source_sha256"]:
                raise ValueError(f"{name}: Bolt input changed between builds")
            report["cases"][name] = {"reference": reference, "expected_stdout": CASES[name], "builds": builds}
        rng = random.Random(args.seed)
        orders = {name: balanced_orders(random.Random(rng.getrandbits(64)), list(variants))
                  for name, variants in programs.items()}
        samples = {name: {label: [] for label in variants} for name, variants in programs.items()}
        for iteration in range(args.warmup + args.runs):
            names = list(programs)
            rng.shuffle(names)
            round_data = []
            for name in names:
                order = next(orders[name])
                times = {label: checked_time(programs[name][label], name, args.timeout) for label in order}
                round_data.append({"case": name, "order": order, "ns": times})
                if iteration >= args.warmup:
                    for label, ns in times.items():
                        samples[name][label].append(ns)
            report["rounds"].append({"warmup": iteration < args.warmup, "samples": round_data})
            if (iteration + 1) % 10 == 0:
                print(f"{iteration + 1}/{args.warmup + args.runs} rounds", flush=True)
        for name, values in samples.items():
            result = report["cases"][name]
            result["timing"] = {key: describe(times) for key, times in values.items()}
            for key in ("before", "c"):
                result[f"after_over_{key}_ci95"] = paired_median_interval(values[key], values["after"], args.seed)
            print(name, {key: row["median_ms"] for key, row in result["timing"].items()})
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
