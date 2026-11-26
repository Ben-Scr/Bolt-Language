"""Time the long UTF-8 round trip and sample the same x64 executables separately."""
import argparse
from bisect import bisect_right
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import winreg

from c_whole_comparison import windows_affinity
from compare import sha256_file
from profile_windows import symbol_table
from release_comparison import run_pair
from suite import CASES, build, cpu_info, paired_median_interval


CASE = "utf8-roundtrip-long"


def profile(exe, sampler, work, minimum):
    starts, names = symbol_table(exe.with_suffix(".exe.linking.map"))

    def symbol(rva):
        if rva <= 0:
            return "[outside executable]"
        index = bisect_right(starts, rva) - 1
        return names[starts[index]] if index >= 0 else "[unknown]"

    functions, locations, stacks = Counter(), Counter(), Counter()
    pc, frames = work / "pcs.tsv", work / "frames.tsv"
    launches = []
    while len(launches) < 12 or sum(launches) < minimum:
        if len(launches) >= 192:
            raise RuntimeError(f"only {sum(launches)} samples for {exe}")
        result = subprocess.run([sampler, exe, pc, frames], cwd=exe.parent,
                                text=True, capture_output=True, timeout=120)
        # Status 5 with an empty stack file means the program exited before
        # the first unwind. Keep its launch in the record and continue sampling.
        if result.stdout.strip() != CASES[CASE] or (result.returncode and
                not (result.returncode == 5 and frames.is_file() and frames.stat().st_size == 0)):
            raise RuntimeError(f"{exe}: {result.returncode} {result.stdout!r} {result.stderr!r}")
        count = 0
        for line in pc.read_text().splitlines():
            module, rva, n = line.split("\t")
            locations[f"{module}+0x{int(rva):x}"] += int(n)
            leaf = symbol(int(rva)) if module.lower() == exe.name.lower() else module
            functions[leaf] += int(n)
            count += int(n)
        launches.append(count)
        for line in frames.read_text().splitlines():
            n, leaf, *returns = map(int, line.split())
            trace = [symbol(leaf), *(symbol(rva - 1) for rva in returns if rva)]
            stacks[" <- ".join(trace)] += n
    if not stacks:
        raise RuntimeError(f"no call stacks for {exe}")
    return {"samples": sum(launches), "samples_per_launch": launches,
            "leaf": dict(functions.most_common()), "pc_rvas": dict(locations.most_common()),
            "stacks": dict(stacks.most_common())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("before", "after", "cc", "sampler", "json"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--runs", type=int, default=32)
    parser.add_argument("--warmup", type=int, default=4)
    parser.add_argument("--seed", type=int, default=19793)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--pin-cpu", type=int, required=True)
    parser.add_argument("--min-samples", type=int, default=512)
    args = parser.parse_args()
    if args.runs < 15 or args.warmup < 0 or args.pin_cpu < 0 or args.min_samples < 1:
        parser.error("require runs >= 15, warmup >= 0, pin-cpu >= 0 and min-samples > 0")
    original, set_mask = windows_affinity()
    if not original & (1 << args.pin_cpu):
        parser.error("selected CPU is outside process affinity")
    cc, sampler = args.cc.resolve(), args.sampler.resolve()
    os.environ["LINK"] = os.environ.get("LINK", "") + " /MAP"
    flags = ["-march=native", "-ffp-contract=off", "-Wl,/map:program.exe.linking.map"]
    cfg = SimpleNamespace(timeout=args.timeout, cflag=flags, boltflag=[], codegen_dir=None, debug_backend="auto")
    report = {"utc": datetime.now(timezone.utc).isoformat(), "host": cpu_info(),
              "case": CASE, "warmup": args.warmup, "runs": args.runs, "seed": args.seed,
              "pin_cpu": args.pin_cpu, "original_affinity_mask": hex(original),
              "method": "whole-process wall time, balanced randomized blocks, stdout checked",
              "cc_version": subprocess.check_output([cc, "--version"], text=True).strip(),
              "builds": {}, "timing": {}, "profile": {}}
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
        report["host"]["cpu_name"] = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    with tempfile.TemporaryDirectory(prefix="bolt-utf8-measure-") as folder:
        work, programs = Path(folder), {}
        for label, sdk in (("before", args.before.resolve()), ("after", args.after.resolve())):
            exe, data = build(CASE, "release", None, sdk, work / label, cfg)
            data["compiler_sha256"] = sha256_file(sdk)
            data["aot_runtime_sha256"] = sha256_file(sdk.parent / "BoltAotRuntime.lib")
            programs[label], report["builds"][label] = exe, data
        for label in ("c_simd", "c_known_edit"):
            if label == "c_known_edit":
                cfg.cflag = [*flags, "-DBENCH_UTF8_KNOWN_EDIT=1"]
            exe, data = build(CASE + "-simd", "c", cc, None, work / label, cfg)
            programs[label], report["builds"][label] = exe, data
        try:
            for series, mask in (("normal", original), ("pinned", 1 << args.pin_cpu)):
                set_mask(mask)
                timed = run_pair(CASE, programs, args)
                rows = [row["elapsed_ms"] for row in timed["rounds"] if not row["warmup"]]
                for ref in ("c_simd", "c_known_edit"):
                    timed[f"after_over_{ref}_ratio_ci95"] = paired_median_interval(
                        [row[ref] for row in rows], [row["after"] for row in rows], args.seed)
                report["timing"][series] = timed
                print(series, {key: timed[key]["median_ms"] for key in programs}, flush=True)
        finally:
            set_mask(original)
        report["sampling"] = {"method": "main thread, nominal 1 ms wall interval, sampled PC and stack",
                              "min_samples": args.min_samples, "sampler_sha256": sha256_file(sampler)}
        for label in programs:
            data = profile(programs[label], sampler, work, args.min_samples)
            report["profile"][label] = data
            print(label, data["samples"], list(data["leaf"].items())[:4], flush=True)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")


if __name__ == "__main__":
    main()
