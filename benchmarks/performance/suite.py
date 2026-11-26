#!/usr/bin/env python3
"""Build correctness-checked C and Bolt programs, then time their whole-process runtime."""
import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

from compare import describe, executable_path, measure, sha256_file, windows_affinity

CASES = {
    "text-encoding": "1825623425",
    "struct-copy-small": "441344",
    "struct-copy-wide": "1695744",
    "string-affixes": "3000000",
    "string-concat": "4264897521",
    "list-fill-clear": "267386880",
    "list-materialized": "1072692801\n1072693400",
    "buffer-record-add": "534773760",
    "buffer-record-add-long": "2139095040",
    "arithmetic": "17925543158443646234",
    "pointers": "3495758670\n313794096",
    "memory": "63747311",
    "memory-long": "509997311",
    "buffer": "17179607040",
    "buffer-long": "4400059777024",
    "buffer-truncate": "2854466355082",
    "buffer-truncate-long": "3902022818554",
    "struct-buffer": "1376475264",
    "struct-zero": "1774290578",
    "queue-worklist": "262063072224",
    "utf8-index-long": "1632783826944",
    "list-edit-long": "7249103406\n59953795191145",
    "dictionary-string-long": "120242307072",
    "queue-worklist-long": "3932022988769",
    "list-search-long": "1073348608",
    "list-search-u64": "1073348608",
    "utf8-roundtrip-long": "409522176",
    "struct-values": "3957781007\n3107464877",
    "float-parse": "14337334190765761419",
    "fp-integrate": "4628760643300657334",
    "indirect-array": "1572842680832",
    "string-equality": "10862172",
    "utf8-roundtrip": "31294464",
    "utf8-index": "6378061824",
    "list-search": "8395412",
    "list-edit": "173240731\n9176763356425",
    "dictionary-string": "3757572096",
}
# Explicitly tuned C sources use the identical Bolt program and oracle.
TUNED_REFERENCES = {
    "pointers-mask": "pointers",
    "buffer-fused": "buffer",
    "buffer-long-fused": "buffer-long",
    "buffer-long-fused-unroll8": "buffer-long",
    "indirect-array-unchecked": "indirect-array",
    "memory-long-prefix": "memory-long",
    "list-search-simd": "list-search",
    "list-search-simd-unroll4": "list-search",
    "utf8-roundtrip-simd": "utf8-roundtrip",
    "utf8-roundtrip-long-simd": "utf8-roundtrip-long",
    "list-search-long-simd-unroll4": "list-search-long",
    "list-search-long-avx2": "list-search-long",
    "dictionary-string-long-cached": "dictionary-string-long",
    "queue-worklist-long-ring": "queue-worklist-long",
    "queue-worklist-ring": "queue-worklist",
    "dictionary-string-cached": "dictionary-string",
}
CASES.update({name: CASES[original] for name, original in TUNED_REFERENCES.items()})
DEFAULT_CASES = ("arithmetic", "pointers", "memory", "buffer")

# The short controls expose process startup. The same sources with more
# iterations give steady-state comparisons without changing their algorithms.
LONG_FIXTURES = {
    # Four full runs still fit the source benchmarks' signed int accumulator.
    "buffer-record-add-long": ("buffer-record-add", b"pass < 4096;", b"pass < 16384;"),
    "list-edit-long": ("list-edit", b"step < 12000;", b"step < 144000;"),
    "dictionary-string-long": ("dictionary-string", b"round < 1024;", b"round < 32768;"),
    "queue-worklist-long": ("queue-worklist", b"turn < 8000000;", b"turn < 120000000;"),
    "list-search-long": ("list-search", b"i < 4096;", b"i < 524288;"),
    "utf8-roundtrip-long": ("utf8-roundtrip", b"round < 768;", b"round < 12288;"),
    "utf8-index-long": ("utf8-index", b"round < 384;", b"round < 98304;"),
    "buffer-truncate-long": ("buffer-truncate", b"pass < 4096;", b"pass < 262144;"),
}


# Longer inputs for short operations and strongly optimized C references.
# The regular --all suite uses the original bounds and fixed answers.
SUSTAINED_BOUNDS = {
    "struct-copy-wide": (b"pass < 2048;", b"pass < 262147;"),
    "queue-worklist": (b"turn < 8000000;", b"turn < 240000000;"),
    "list-search": (b"i < 4096;", b"i < 1048576;"),
    "utf8-roundtrip": (b"round < 768;", b"round < 49152;"),
    "float-parse": (b"i < 1000000;", b"i < 3000031;"),
    "string-equality": (b"i < 4000000;", b"i < 48000000;"),
    "struct-buffer": (b"frame < 1024;", b"frame < 65536;"),
    "struct-zero": (b"frame < 256;", b"frame < 4096;"),
}
SUSTAINED_CASES = {
    "struct-copy-wide": "1696767",
    "queue-worklist": "7864123242860",
    "queue-worklist-ring": "7864123242860",
    "list-search-simd-unroll4": "2146697216",
    "utf8-roundtrip-simd": "1620154368",
    "float-parse": "11358106450416173224",
    "string-equality": "3996488",
    "struct-buffer": "90453590232",
    "struct-zero": "28226804265",
}

PROFILES = ("c", "debug", "release", "cpp-release")
ROOT = Path(__file__).resolve().parent


def cpu_info():
    name = platform.processor()
    features = None
    if Path("/proc/cpuinfo").is_file():
        fields = {}
        for line in Path("/proc/cpuinfo").read_text().split("\n\n", 1)[0].splitlines():
            key, separator, value = line.partition(":")
            if separator:
                fields[key.strip()] = value.strip()
        name = fields.get("model name", name)
        features = fields.get("flags", fields.get("Features"))
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    if os.name == "nt":
        import winreg
        mask, _ = windows_affinity()
        affinity = [cpu for cpu in range(mask.bit_length()) if mask & (1 << cpu)]
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            name = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    return {"platform": platform.platform(), "machine": platform.machine(), "cpu": name,
            "advertised_features": features, "python": platform.python_version(),
            "cpu_affinity": affinity}


def captured(command, cwd, timeout):
    start = time.perf_counter_ns()
    proc = subprocess.run(command, cwd=cwd, capture_output=True, text=True, errors="replace",
                          timeout=timeout, check=False)
    elapsed = time.perf_counter_ns() - start
    log = (proc.stdout + "\n" + proc.stderr).strip()
    if proc.returncode:
        raise RuntimeError(f"exit {proc.returncode}: {log[-4000:]}")
    return elapsed, log


def build(case, profile, cc, bolt, directory, args, *, collect_codegen=True):
    label = profile
    local = directory / case / label
    if profile == "before-release":
        profile = "release"
    local.mkdir(parents=True)
    extension = ".c" if profile == "c" else ".bolt"
    base_case = TUNED_REFERENCES.get(case, case)
    fixture = "memory" if base_case == "memory-long" else base_case
    if base_case in LONG_FIXTURES:
        fixture = LONG_FIXTURES[base_case][0]
    if profile == "c" and case in {"pointers-mask", "buffer-fused", "buffer-long-fused"}:
        fixture = case
    if profile == "c" and case == "buffer-long-fused-unroll8":
        fixture = "buffer-long-fused"
    avx_search = profile == "c" and case in {"list-search-long-avx2", "list-search-u64"}
    if avx_search:
        # Pass --cflag=-mavx2 (MSVC: --cflag=/arch:AVX2) on capable hosts.
        # The same C fixture also has a portable scalar path.
        fixture = "list-search-avx2"
    source = ROOT / "cases" / (fixture + extension)
    src = local / (case + extension)
    body = source.read_bytes()
    if avx_search and case == "list-search-u64":
        body = b"#define BENCH_WIDE_SEARCH 1\n" + body
    if getattr(args, "sustained", False):
        initial, extended = SUSTAINED_BOUNDS[base_case]
        if body.count(initial) != 1:
            raise RuntimeError(f"expected the regular loop bound in {source.name}")
        body = body.replace(initial, extended)
        if base_case == "queue-worklist":
            # The longer input reaches integers above INT32_MAX / 17.
            # Cast the turn before multiplication in both implementations.
            cast, safe = ((b"(uint32_t)(turn * 17)", b"((uint32_t)turn * UINT32_C(17))")
                          if profile == "c" else (b"(uint)(turn * 17)", b"((uint)turn * 17u)"))
            if body.count(cast) != 1:
                raise RuntimeError(f"expected the queue state expression in {source.name}")
            body = body.replace(cast, safe)
    if base_case in LONG_FIXTURES and not avx_search:
        _, original, long_bound = LONG_FIXTURES[base_case]
        if body.count(original) != 1:
            raise ValueError(f"expected the short loop bound in {source.name}")
        body = body.replace(original, long_bound)
    if profile == "c":
        controls = {
            "list-search-simd": b"BENCH_VECTOR_SEARCH",
            "list-search-simd-unroll4": b"BENCH_VECTOR_SEARCH",
            "utf8-roundtrip-simd": b"BENCH_UTF8_SIMD",
            "utf8-roundtrip-long-simd": b"BENCH_UTF8_SIMD",
            "list-search-long-simd-unroll4": b"BENCH_VECTOR_SEARCH",
            "dictionary-string-long-cached": b"BENCH_CACHED_HASH",
            "queue-worklist-long-ring": b"BENCH_RING_QUEUE",
            "dictionary-string-cached": b"BENCH_CACHED_HASH",
            "queue-worklist-ring": b"BENCH_RING_QUEUE",
        }
        if case in controls:
            body = b"#define " + controls[case] + b" 1\n" + body
        if case in {"list-search-simd-unroll4", "list-search-long-simd-unroll4"}:
            body = b"#define BENCH_SEARCH_UNROLL4 1\n" + body
        if case in {"utf8-roundtrip-simd","utf8-roundtrip-long-simd"}:
            header = ROOT.parents[1] / "include" / "bolt" / "detail" / "Utf8Block.h"
            shutil.copyfile(header, local / header.name)
    if profile == "c" and case == "buffer-long-fused-unroll8":
        loop = b"        for (int i = 0; i < 524288; ++i)"
        if body.count(loop) != 1:
            raise RuntimeError(f"expected the fusion control's inner loop in {source.name}")
        body = body.replace(loop, b"        #pragma GCC unroll 8\n" + loop)
    if base_case == "memory-long":
        # Use the same versioned pair; only the number of rounds differs.
        short_loop = b"step < 250000;"
        if body.count(short_loop) != 1:
            raise RuntimeError(f"expected exactly one memory-loop bound in {source.name}")
        body = body.replace(short_loop, b"step < 2000000;")
    if profile == "c" and case == "memory-long-prefix":
        assert body.count(b"memcmp(a, b, n)") == 1
        body = body.replace(b"memcmp(a, b, n)", b"((int)a[n - 1] - (int)b[n - 1])")
    if profile == "c" and case == "indirect-array-unchecked":
        for check in (b"        if ((uint32_t)cursor >= 65536) return 2;\n",
                      b"        if (cursor < 0) return 3;\n"):
            assert body.count(check) == 1
            body = body.replace(check, b"")
    src.write_bytes(body)
    exe_name = "program.exe" if os.name == "nt" else "program"
    if profile == "c":
        if cc.stem.lower() == "cl":
            command = [str(cc), "/nologo", "/O2", "/DNDEBUG", "/std:c11", *args.cflag,
                       src.name, "/Fe:" + exe_name]
        else:
            command = [str(cc), "-std=c11", "-O3", "-DNDEBUG", *args.cflag, src.name, "-o", exe_name]
    else:
        backend = {"debug": args.debug_backend, "release": "llvm", "cpp-release": "cpp"}[profile]
        config = "--debug" if profile == "debug" else "--release"
        command = [str(bolt), "compile", src.name, config, "--backend", backend]
        if backend != "cpp":
            command.append("--require-native")
        command += [*args.boltflag, "-o", exe_name]
    elapsed, log = captured(command, local, args.timeout)
    exe = local / exe_name
    data = {
        "fixture": source.name, "fixture_sha256": sha256_file(source),
        "command": command, "source_sha256": sha256_file(src), "executable_sha256": sha256_file(exe),
        "build_ms": elapsed / 1e6, "build_log": log,
        "runtime_sha256": {f.name: sha256_file(f) for f in local.glob("*BoltRuntime*") if f.is_file()},
    }
    if profile == "c" and case in {"utf8-roundtrip-simd","utf8-roundtrip-long-simd"}:
        data["header_sha256"] = {header.name: sha256_file(header)}
    if profile == "release":
        aot_runtime = bolt.parent / "BoltAotRuntime.o"
        if aot_runtime.is_file():
            data["aot_runtime_sha256"] = sha256_file(aot_runtime)
    if collect_codegen and args.codegen_dir is not None:
        output = args.codegen_dir.resolve() / case / label
        output.mkdir(parents=True, exist_ok=True)
        disassembler = shutil.which(args.objdump)
        if not disassembler:
            raise RuntimeError(f"--codegen-dir needs a disassembler: {args.objdump}")
        _, assembly = captured([disassembler, "-d", str(exe)], local, args.timeout)
        (output / "program.asm").write_text(assembly + "\n", encoding="utf-8")
        if profile in ("release", "debug"):
            config = "--debug" if profile == "debug" else "--release"
            captured([str(bolt), "emit-ir", src.name, config, *args.boltflag, "-o", str(output / "program.ll")],
                     local, args.timeout)
        data["codegen_dir"] = str(output)
    return exe, data


def paired_median_interval(c_times, bolt_times, seed):
    """Bootstrap complete measurement pairs to retain shared scheduling effects."""
    rng = random.Random(seed)
    ratios = []
    for _ in range(4000):
        indices = rng.choices(range(len(c_times)), k=len(c_times))
        ratios.append(statistics.median(bolt_times[i] for i in indices) /
                      statistics.median(c_times[i] for i in indices))
    ratios.sort()
    return [ratios[99], ratios[3899]]


def checked_run(exe, expected, timeout):
    measured = measure(exe, [], timeout)
    if measured["stdout"] != expected:
        raise ValueError(f"expected {expected!r}, got {measured['stdout']!r}")
    return measured["time_ns"]


def run_case(case, args, cc, compilers, directory, rng, unavailable):
    expected = (SUSTAINED_CASES if getattr(args, "sustained", False) else CASES)[case]
    report = {"expected_stdout": expected, "profiles": {}, "build_order": [], "measurement_order": []}
    builds = {}
    build_times = {}
    for profile in args.profiles:
        missing = unavailable.get(profile)
        if missing:
            report["profiles"][profile] = {"error": missing}
        else:
            build_times[profile] = []
    # One untimed warm-up for repeated builds. Every compiler invocation sees a
    # fresh source directory with no cached objects, executable or project state.
    build_warmup = int(args.build_runs > 1)
    for round_index in range(build_warmup + args.build_runs):
        final_build = round_index == build_warmup + args.build_runs - 1
        order = list(build_times)
        if args.build_runs > 1:
            rng.shuffle(order)
        report["build_order"].append(order)
        with (nullcontext(directory) if final_build else tempfile.TemporaryDirectory(dir=directory)) as work:
            for profile in order:
                try:
                    path, metadata = build(case, profile, cc, compilers.get(profile), Path(work), args,
                                           collect_codegen=final_build)
                    if final_build:
                        builds[profile] = path
                        report["profiles"][profile] = metadata
                    else:
                        checked_run(path, expected, args.timeout)
                    if round_index >= build_warmup:
                        build_times[profile].append(round(metadata["build_ms"] * 1e6))
                except (ValueError, OSError, subprocess.SubprocessError, RuntimeError) as error:
                    report["profiles"][profile] = {"error": str(error)}
                    del build_times[profile]
    durations = {profile: [] for profile in builds}
    for profile in builds:
        report["profiles"][profile]["compile_link"] = describe(build_times[profile])
    for i in range(args.warmup + args.runs):
        order = list(builds)
        rng.shuffle(order)
        if i >= args.warmup:
            report["measurement_order"].append(order)
        for profile in order:
            try:
                elapsed = checked_run(builds[profile], expected, args.timeout)
                if i >= args.warmup:
                    durations[profile].append(elapsed)
            except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
                report["profiles"][profile]["error"] = str(error)
                del builds[profile]
    failed = len(builds) != len(args.profiles)
    for profile in builds:
        times = describe(durations[profile])
        report["profiles"][profile].update(times)
        compile_ms = report["profiles"][profile]["compile_link"]["median_ms"]
        print(f"{case:12} {profile:12} build {compile_ms:9.3f} ms, run median {times['median_ms']:10.3f} ms, "
              f"p95 {times['p95_ms']:10.3f} ms", flush=True)
    for profile, data in report["profiles"].items():
        if "error" in data:
            print(f"{case}/{profile}: {data['error']}", file=sys.stderr, flush=True)
    if "c" in builds and "release" in builds:
        c_ns = statistics.median(durations["c"])
        bolt_ns = statistics.median(durations["release"])
        qualified = min(c_ns, bolt_ns) >= args.min_duration_ms * 1e6 and args.runs >= 15 and args.warmup >= 1
        report["c_over_release"] = {"speedup": c_ns / bolt_ns, "valid_duration_and_sample_count": qualified}
        if len(durations["c"]) >= 15:
            report["c_over_release"]["bolt_over_c_median_ci95_paired_bootstrap"] = paired_median_interval(
                durations["c"], durations["release"], args.seed)
        c_p95 = report["profiles"]["c"]["p95_ms"]
        bolt_p95 = report["profiles"]["release"]["p95_ms"]
        report["c_over_release"]["p95_speedup"] = c_p95 / bolt_p95
        if args.min_p95_speedup is not None and (not qualified or c_p95 / bolt_p95 < args.min_p95_speedup):
            report["c_over_release"]["p95_gate_failed"] = True
            failed = True
        if args.min_speedup is not None and (not qualified or c_ns / bolt_ns < args.min_speedup):
            report["c_over_release"]["gate_failed"] = True
            failed = True
    return report, failed


def main():
    p = argparse.ArgumentParser(description=__doc__)
    selected = p.add_mutually_exclusive_group()
    selected.add_argument("--cases", nargs="+", choices=CASES)
    selected.add_argument("--all", action="store_true", help="Run all regular workloads and tuned C controls")
    selected.add_argument("--regular", action="store_true", help="Run each regular workload with its regular C implementation")
    p.add_argument("--sustained", action="store_true", help="Use longer inputs for eight operation and C-control comparisons")
    p.add_argument("--profiles", nargs="+", choices=PROFILES, default=["c", "release"])
    p.add_argument("--bolt", default="bolt")
    p.add_argument("--before", help="Also measure an earlier Bolt Release SDK in the same rounds")
    p.add_argument("--cc", default="cl" if os.name == "nt" else "cc")
    p.add_argument("--debug-backend", choices=["auto", "fast", "llvm"], default="auto")
    p.add_argument("--cflag", action="append", default=[], help="Additional C compiler flag, e.g. --cflag=-msse2")
    p.add_argument("--boltflag", action="append", default=[], help="Additional flag, e.g. --boltflag=--define")
    p.add_argument("--codegen-dir", type=Path, help="Keep disassembly and direct Bolt IR before LLVM optimization")
    p.add_argument("--objdump", default="objdump", help="GNU objdump or llvm-objdump for --codegen-dir")
    p.add_argument("--warmup", type=int, default=2)
    p.add_argument("--runs", type=int, default=15)
    p.add_argument("--build-runs", type=int, default=1,
                   help="Fresh compile-and-link samples per profile (one extra warm-up if repeated)")
    p.add_argument("--timeout", type=float, default=120)
    p.add_argument("--seed", type=int, default=442109)
    p.add_argument("--min-duration-ms", type=float, default=200)
    p.add_argument("--min-speedup", type=float, help="Fail a qualifying C/release pair below this speedup; short samples also fail")
    p.add_argument("--min-p95-speedup", type=float, help="Optional p95 C/release gate using the same qualification criteria")
    p.add_argument("--json", type=Path, required=True)
    args = p.parse_args()
    selection = SUSTAINED_CASES if args.sustained else CASES
    if args.regular:
        args.cases = [case for case in selection if case not in TUNED_REFERENCES]
    elif args.all or (args.sustained and args.cases is None):
        args.cases = list(selection)
    elif args.cases is None:
        args.cases = list(DEFAULT_CASES)
    if any(case not in selection for case in args.cases):
        p.error("--sustained supports: " + ", ".join(SUSTAINED_CASES))
    if args.runs < 1 or args.build_runs < 1 or args.warmup < 0 or args.timeout <= 0 or args.min_duration_ms < 0:
        p.error("invalid run count, timeout, or duration")
    if args.min_speedup is not None and (args.min_speedup <= 0 or not {"c", "release"} <= set(args.profiles)):
        p.error("--min-speedup requires c and release profiles and a positive value")
    if args.min_p95_speedup is not None and (args.min_p95_speedup <= 0 or not {"c", "release"} <= set(args.profiles)):
        p.error("--min-p95-speedup requires c and release profiles and a positive value")
    if len(args.cases) != len(set(args.cases)) or len(args.profiles) != len(set(args.profiles)):
        p.error("duplicate case or profile")
    if args.before and "release" not in args.profiles:
        p.error("--before requires the release profile")
    compiler_info = {}
    cc = None
    compilers = {}
    unavailable = {}
    if "c" in args.profiles:
        try:
            cc = executable_path(args.cc)
            flags = [] if cc.stem.lower() == "cl" else ["--version"]
            # MSVC prints its banner to stderr and some versions return nonzero without inputs.
            probe = subprocess.run([str(cc), *flags], capture_output=True, text=True, errors="replace", timeout=args.timeout)
            compiler_info["c"] = (probe.stdout + "\n" + probe.stderr).strip()
            compiler_info["c_sha256"] = sha256_file(cc)
        except (OSError, subprocess.SubprocessError) as error:
            unavailable["c"] = str(error)
    sdks = []
    bolt_profiles = [profile for profile in args.profiles if profile != "c"]
    if bolt_profiles:
        sdks.append(("bolt", args.bolt, bolt_profiles))
    if args.before:
        args.profiles = [*args.profiles, "before-release"]
        sdks.append(("before", args.before, ["before-release"]))
    for label, spec, profiles in sdks:
        try:
            bolt = executable_path(spec)
            for cmd in ("version", "backends"):
                compiler_info[label + "_" + cmd] = captured([str(bolt), cmd], None, args.timeout)[1]
            compiler_info[label + "_sha256"] = sha256_file(bolt)
            compiler_info[label + "_sdk_sha256"] = {
                name: sha256_file(bolt.parent / name)
                for name in ("BoltCompiler.dll", "BoltAotRuntime.lib", "BoltAotRuntime.o")
                if (bolt.parent / name).is_file()
            }
            compilers.update(dict.fromkeys(profiles, bolt))
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            unavailable.update(dict.fromkeys(profiles, str(error)))
    output = {"schema": 1, "utc": datetime.now(timezone.utc).isoformat(), "host": cpu_info(),
              "toolchains": compiler_info, "metric": "whole process wall time including startup and output",
              "environment": {key: os.environ[key] for key in ("BOLT_LINKER", "BOLT_LLVM_LIBRARY") if key in os.environ},
              "seed": args.seed, "warmup": args.warmup, "runs": args.runs, "timeout_s": args.timeout,
              "build_runs": args.build_runs, "build_warmup": int(args.build_runs > 1),
              "min_duration_ms": args.min_duration_ms, "min_speedup": args.min_speedup,
              "workload": "sustained" if args.sustained else "regular", "cases": {}}
    rng = random.Random(args.seed)
    failed = False
    with tempfile.TemporaryDirectory(prefix="bolt-suite-") as temp:
        for case in args.cases:
            result, bad = run_case(case, args, cc, compilers, Path(temp), rng, unavailable)
            output["cases"][case] = result
            failed |= bad
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (FileNotFoundError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
