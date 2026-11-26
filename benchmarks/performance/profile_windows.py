"""Sample complete single-threaded LLVM Release workloads with the Windows x64 sampler."""
import argparse
from bisect import bisect_right
from collections import Counter
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from types import SimpleNamespace

from compare import sha256_file
from suite import CASES, TUNED_REFERENCES, build, cpu_info


def symbol_table(path):
    content = path.read_text(errors="replace")
    base = int(re.search(r"Preferred load address is ([0-9a-fA-F]+)", content)[1], 16)
    symbols = {}
    public = True
    for line in content.splitlines():
        if line.strip() == "Static symbols":
            public = False
        # lld-link omits the "f" flag for Clang code and CRT objects. Keep
        # public CRT entry points, avoiding internal assembler branch labels.
        match = re.match(r"\s+0001:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.+)", line)
        if match and (match[3].startswith("f ") or match[3].endswith(".o")
                      or (public and match[3].endswith(".obj"))):
            name = match[1]
            if name.startswith("b_"):
                name = re.sub(r"_([0-9a-f]{2})", lambda m: chr(int(m[1], 16)), name)
            symbols.setdefault(int(match[2], 16) - base, name)
    return sorted(symbols), symbols


def profile(exe, sampler, expected, rounds, minimum_samples, temp):
    starts, names = symbol_table(exe.with_suffix(".exe.linking.map"))
    functions, locations = Counter(), Counter()
    launches = 0
    while launches < rounds or (sum(functions.values()) < minimum_samples and launches < 16 * rounds):
        launches += 1
        path = temp / "samples.tsv"
        result = subprocess.run([sampler, exe, path], text=True, capture_output=True, timeout=120,
                                cwd=exe.parent, check=True)
        if result.stdout.strip() != expected:
            raise ValueError(f"wrong result for {exe}: {result.stdout!r}")
        for line in path.read_text().splitlines():
            module, offset, count = line.split("\t")
            offset, count = int(offset), int(count)
            locations[f"{module}+0x{offset:x}"] += count
            name = module
            if module.lower() == exe.name.lower():
                index = bisect_right(starts, offset) - 1
                name = names[starts[index]] if index >= 0 else module
            functions[name] += count
    return {"launches": launches, "samples": sum(functions.values()), "functions": dict(functions.most_common()),
            "pc_rvas": dict(locations.most_common()), "executable_sha256": sha256_file(exe)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", type=Path, required=True)
    parser.add_argument("--sampler", type=Path, required=True)
    parser.add_argument("--programs", type=Path, help="reuse an existing suite build tree with linker maps")
    parser.add_argument("--runs", type=int, default=8)
    parser.add_argument("--min-samples", type=int, default=100, help="repeat short programs up to 16 times the run count")
    parser.add_argument("--cases", nargs="+", choices=CASES)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.min_samples < 0:
        parser.error("require --runs > 0 and --min-samples >= 0")
    sdk, sampler = args.bolt.resolve(), args.sampler.resolve()
    opts = SimpleNamespace(boltflag=[], timeout=120, codegen_dir=None, debug_backend="auto")
    os.environ["LINK"] = os.environ.get("LINK", "") + " /MAP"
    report = {"host": cpu_info(), "compiler_sha256": sha256_file(sdk), "runs": args.runs,
              "min_samples": args.min_samples, "sampling": "main thread, nominal 1 ms wall interval, PC only", "cases": {}}
    for case in args.cases or [c for c in CASES if c not in TUNED_REFERENCES]:
        with tempfile.TemporaryDirectory(prefix="bolt-samples-") as work:
            temp = Path(work)
            if args.programs:
                exe = args.programs.resolve() / case / "release" / "program.exe"
            else:
                exe, _ = build(case, "release", None, sdk, temp, opts)
            data = profile(exe, sampler, CASES[case], args.runs, args.min_samples, temp)
            report["cases"][case] = data
            print(case, data["samples"], list(data["functions"].items())[:3], flush=True)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")


if __name__ == "__main__":
    main()
