"""Measure paired and individual List<int> edits against an optimized C array."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent
BATCH = 64


def stride(size, start):
    # Visit all size+1 legal positions, including append and remove-last.
    while math.gcd(start, size+1) != 1:
        start += 1
    return start


def program(size, mode, language):
    if mode == "individual":
        text = (ROOT / ("individual." + language)).read_text()
        return (text.replace("int size = 64;", f"int size = {size};")
                .replace("round < 4096;", f"round < {rounds(size)};")
                .replace("step * 117", f"step * {stride(size,117)}")
                .replace("step * 251", f"step * {stride(size,251)}"))
    text = (ROOT.parent / "cases" / ("list-edit." + language)).read_text()
    index = f"& {size-1}" if size & (size-1) == 0 else f"% {size}"
    text = text.replace("& 32767", index).replace("32769", str(size+1)).replace("32768", str(size))
    text = text.replace("step < 12000;", f"step < {pairs(size)};")
    if mode == "total":
        return text
    if language == "bolt":
        return ("import Bolt.Runtime\n" + text).replace("        for (int step",
            "        long start = Clock.MonotonicTicks();\n        for (int step").replace("        ulong final",
            "        Print(Clock.MonotonicTicks() - start);\n        ulong final")
    return ('#include "timer.h"\n' + text).replace("    for (int32_t step",
        "    int64_t start = bench_now();\n    for (int32_t step").replace("    uint64_t final",
        '    printf("%" PRId64 "\\n", bench_to_ticks(bench_now()-start));\n    uint64_t final')


def pairs(size):
    return 250000 if size <= 1024 else 12000


def rounds(size):
    return min(4096, 4194304 // size)


def reference(size, individual):
    lists = [list(range(size)) for _ in range(BATCH if individual else 1)]
    removed = 0
    p_stride, q_stride = (stride(size,117), stride(size,251)) if individual else (117,251)
    for i in range(rounds(size)*BATCH if individual else pairs(size)):
        modulus = size+1 if individual else size
        items = lists[i % len(lists)]
        items.insert((i*p_stride+257) % modulus, i//BATCH if individual else i)
        removed += items.pop((i*q_stride+11) % modulus)
    return [removed, sum((i+1)*v for items in lists for i, v in enumerate(items))]


def measure(args, work):
    compilers = {"Clang": args.cc, "Bolt": args.bolt}
    if args.before:
        compilers["Before"] = args.before
    compilers = {k: str(Path(shutil.which(v) or v).resolve(strict=True)) for k, v in compilers.items()}
    cases = [(n, mode) for n in args.sizes for mode in args.modes]
    expected = {(n, ind): reference(n, ind) for n, ind in {(n, m == "individual") for n, m in cases}}
    binaries = {}
    for size, mode in cases:
        folder = work / str(size) / mode
        folder.mkdir(parents=True, exist_ok=True)
        for label, compiler in compilers.items():
            language = "c" if label == "Clang" else "bolt"
            source = folder / ("Program." + language)
            source.write_text(program(size, mode, language))
            output = folder / (label + (".exe" if platform.system() == "Windows" else ""))
            if language == "c":
                command = [compiler, "-std=c11", "-O3", "-DNDEBUG"]
                if mode != "total" and platform.system() != "Windows":
                    command.append("-D_POSIX_C_SOURCE=200809L")
                command += ["-I", str(ROOT), str(source), "-o", str(output)]
            else:
                command = [compiler, "compile", str(source), "--rebuild", "--release", "--backend", "llvm",
                           "--require-native", "-o", str(output)]
            subprocess.run(command, cwd=folder, check=True, stdout=subprocess.DEVNULL)
            binaries[size, mode, label] = output

    rng = random.Random(args.seed)
    data = {str(n): {} for n in args.sizes}
    for rep in range(args.warmup + args.runs):
        for size, mode in rng.sample(cases, len(cases)):
            for label in rng.sample(list(compilers), len(compilers)):
                start = time.perf_counter_ns()
                output = subprocess.check_output([str(binaries[size, mode, label])], cwd=work, timeout=120)
                wall = (time.perf_counter_ns()-start) / 1e6
                values = [int(s) for s in output.splitlines()]
                phases = {"total": [], "phase": ["pair"], "individual": ["insert", "remove"]}[mode]
                if len(values) != len(phases)+2 or values[-2:] != expected[size, mode == "individual"]:
                    raise ValueError(f"incorrect output for {size}/{mode}/{label}: {values}")
                if rep >= args.warmup:
                    sample = {p: ticks/10000 for p, ticks in zip(phases, values)}
                    if mode == "total":
                        sample["total"] = wall
                    for p, value in sample.items():
                        data[str(size)].setdefault(p, {}).setdefault(label, []).append(value)
    medians = {n: {p: {k: statistics.median(v) for k, v in labels.items()}
                   for p, labels in row.items()} for n, row in data.items()}
    result = {"unit": "ms", "seed": args.seed, "warmup": args.warmup, "runs": args.runs,
              "host": platform.platform(), "compiler_versions": {
                  k: subprocess.check_output([v, "--version"], text=True).splitlines()[0] for k, v in compilers.items()},
              "compiler_sha256": {k: hashlib.sha256(Path(v).read_bytes()).hexdigest() for k, v in compilers.items()},
              "windows_aot_runtime_sha256": {
                  k: hashlib.sha256(runtime.read_bytes()).hexdigest() for k, v in compilers.items()
                  if k != "Clang" and (runtime := Path(v).with_name("BoltAotRuntime.lib")).is_file()},
              "counts": {n: {"pairs": pairs(n), "individual_per_method": BATCH*rounds(n)} for n in args.sizes},
              "medians": medians, "samples": data}
    print(json.dumps(medians, indent=2))
    if args.json:
        args.json.write_text(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", default="bolt")
    parser.add_argument("--before")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--sizes", nargs="+", type=int, default=[4, 8, 16, 31, 64, 128, 256, 1024])
    parser.add_argument("--modes", nargs="+", choices=["total", "phase", "individual"], default=["total", "phase", "individual"])
    parser.add_argument("--runs", type=int, default=21)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--seed", type=int, default=117251)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    if any(not 1 <= n <= 32768 for n in args.sizes) or args.runs < 1 or args.warmup < 0:
        parser.error("sizes must be 1..32768, runs positive and warmup nonnegative")
    with tempfile.TemporaryDirectory(prefix="bolt-list-edit-sizes-") as work:
        measure(args, Path(work))


if __name__ == "__main__":
    main()
