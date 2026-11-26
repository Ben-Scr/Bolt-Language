"""Locate end-to-end differences by timing read, compute and write in separate Release builds."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import shutil
import tempfile

from run import CASES, HERE, ROOT, RELEASE_FLAGS, SEED, c_build, describe, execute, orders, sha256_file


def instrument(source, bolt):
    lines = source.splitlines()
    read = next(i for i, line in enumerate(lines) if ("File.ReadAllBytes(" if bolt else " = read_input(") in line)
    write = next(i for i, line in enumerate(lines) if ("File.WriteAllBytes(" if bolt else "    write_result(") in line)
    if bolt:
        prefix = ["import System.Diagnostics"]
        start = ["Stopwatch watch;", "watch.Start();"]
        mark = lambda n: f"long t{n} = watch.ElapsedTicks;"
        emit = ["Print(t1);", "Print(t2 - t1);", "Print(t3 - t2);"]
    else:
        prefix = ["#include <windows.h>"]
        start = ["LARGE_INTEGER freq, t0, t1, t2, t3;",
                 "QueryPerformanceFrequency(&freq);", "QueryPerformanceCounter(&t0);"]
        mark = lambda n: f"QueryPerformanceCounter(&t{n});"
        emit = [f'printf("%lld\\n", (t{n}.QuadPart - t{n-1}.QuadPart) * 10000000LL / freq.QuadPart);'
                for n in (1, 2, 3)]
    return "\n".join([*prefix, *lines[:read], *start, lines[read], mark(1), *lines[read+1:write],
                      mark(2), lines[write], mark(3), *emit, *lines[write+1:]]) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bolt", type=Path, default=ROOT / "bin/bolt.exe")
    p.add_argument("--clang", type=Path, default=Path(r"C:\Program Files\LLVM\bin\clang.exe"))
    p.add_argument("--json", type=Path, required=True)
    args = p.parse_args()
    if not shutil.which("cl"):
        p.error("Run from an x64 Visual Studio developer shell")
    cc = {"msvc": Path(shutil.which("cl")), "clang": args.clang.resolve(strict=True)}
    bolt = args.bolt.resolve(strict=True)
    manifest = json.loads((HERE / "v1.json").read_text())
    rng = random.Random(SEED)
    report = {"baseline": "v1", "method": "Separate instrumented executables. Inside Main, excluding startup. "
              "Compute includes output allocation. Four warmups, 18 measured runs, all outputs validated.",
              "script_sha256": sha256_file(Path(__file__)), "seed": SEED,
              "tool_hashes": {p.name: sha256_file(p) for p in [bolt, bolt.with_name("BoltRuntime.dll"), *cc.values()]},
              "cases": {}}
    with tempfile.TemporaryDirectory(prefix="bolt-script-phases-") as tmp:
        for name, (generate, _, _) in CASES.items():
            work = Path(tmp) / name
            work.mkdir()
            data, _ = generate()
            assert hashlib.sha256(data).hexdigest() == manifest[name]["input_sha256"]
            (work / "input.bin").write_bytes(data)
            shutil.copy(HERE / "fixture_io.h", work)
            programs = {}
            for lab in ("bolt", *cc):
                d = work / lab
                d.mkdir()
                filename = "Program.bolt" if lab == "bolt" else "main.c"
                src = d / filename
                src.write_text(instrument((HERE / name / filename).read_text(), lab == "bolt"))
                if lab == "bolt":
                    execute([bolt, "compile", filename, *RELEASE_FLAGS, "-o", "program.exe"], d)
                else:
                    c_build(cc[lab], src, d)
                programs[lab] = d / "program.exe"
            samples = {lab: {phase: [] for phase in ("read", "compute", "write")} for lab in programs}
            for i, row in enumerate(orders(programs, 4, rng) + orders(programs, 18, rng)):
                for lab in row:
                    result = work / "result.bin"
                    result.unlink(missing_ok=True)
                    _, out = execute([programs[lab]], work)
                    ticks = [int(line) for line in out.splitlines()]
                    if len(ticks) != 3 or sha256_file(result) != manifest[name]["output_sha256"][0]:
                        raise ValueError(f"Incorrect phase result for {name}/{lab}")
                    if i >= 4:
                        for phase, tick in zip(samples[lab].values(), ticks):
                            phase.append(tick * 100)
            report["cases"][name] = {lab: {phase: describe(ns) for phase, ns in stages.items()}
                                     for lab, stages in samples.items()}
            print(name, {lab: [round(stat["median_ms"], 2) for stat in stages.values()]
                         for lab, stages in report["cases"][name].items()}, flush=True)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
