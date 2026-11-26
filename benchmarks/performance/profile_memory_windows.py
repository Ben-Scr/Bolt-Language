"""Attribute Windows x64 memory samples to calls in the LLVM workload's bolt_entry."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import tempfile

from compare import sha256_file
from profile_windows import symbol_table
from suite import CASES


OPERATIONS = {**{f"br_memory_{op}": op for op in ("copy", "move", "set", "compare")},
              "memcpy": "copy", "memmove": "move", "memset": "set", "memcmp": "compare"}


def return_sites(exe, objdump):
    # The linker may fold memcpy and memmove together. Their names are still
    # distinct in the COFF relocations at the caller. Linker maps give the RVA.
    _, symbols = symbol_table(Path(str(exe) + ".linking.map"))
    entry = next((rva for rva, name in symbols.items() if name == "bolt_entry"), None)
    asm = subprocess.check_output([objdump, "-dr", "--no-show-raw-insn", str(exe) + ".bolt.obj"],
                                  text=True, encoding="utf8", errors="replace")
    match = re.search(r"^([\da-f]+) <bolt_entry>:\n(.+?)(?:\n\n|\Z)", asm, re.M | re.S)
    if entry is None or match is None:
        raise ValueError(f"bolt_entry missing from the link map or COFF object for {exe}")
    object_entry = int(match[1], 16)
    calls = {}
    for ret, symbol in re.findall(r"\bcallq\s+0x([a-f\d]+) [^\n]*\n\s*[a-f\d]+:"
                                  r"\s+IMAGE_REL_AMD64_REL32\s+(\w+)", match[2]):
        if symbol in OPERATIONS:
            calls[entry + int(ret, 16) - object_entry] = OPERATIONS[symbol]
    return calls


def profile(exe, args):
    calls = return_sites(exe, args.objdump)
    if not calls:
        raise ValueError(f"no memory return sites found in {exe}")
    counts, raw, site_samples, launches = Counter(), Counter(), Counter(), []
    with tempfile.TemporaryDirectory(prefix="bolt-memory-stacks-") as temp:
        pcs, frames = (Path(temp)/f for f in ("pcs.tsv", "frames.tsv"))
        for _ in range(args.runs):
            child = subprocess.run([args.sampler, exe, pcs, frames], capture_output=True, check=True,
                                   text=True, timeout=120, cwd=exe.parent)
            if child.stdout.strip() != CASES["memory-long"]:
                raise ValueError(f"wrong result for {exe}: {child.stdout!r}")
            batch = Counter()
            for line in frames.read_text().splitlines():
                amount, leaf, *parents = map(int, line.split())
                site = next((p for p in parents if p in calls), None)
                batch[calls.get(site, "other")] += amount
                if site is not None:
                    site_samples[hex(site)] += amount
                raw[" ".join(map(hex, [leaf, *parents]))] += amount
            total = sum(int(line.split("\t")[-1]) for line in pcs.read_text().splitlines())
            if total < sum(batch.values()):
                raise ValueError("inconsistent sample counts")
            batch["other"] += total - sum(batch.values())
            counts.update(batch)
            launches.append(dict(batch))
    return {"executable_sha256": sha256_file(exe), "samples": sum(counts.values()), "counts": counts,
            "rounds": launches, "return_sites": {hex(pc): op for pc, op in calls.items()},
            "samples_by_return_site": site_samples, "stacks": raw}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("before", "after", "sampler", "objdump", "json"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--runs", default=8, type=int)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    args.sampler = args.sampler.resolve()
    result = {name: profile(exe.resolve(), args) for name, exe in (("before", args.before), ("after", args.after))}
    for name, info in result.items():
        print(name, info["counts"], flush=True)
    result["method"] = {"runs_per_executable": args.runs, "nominal_interval_ms": 1,
                        "thread": "main", "caller": "x64 unwind and COFF call relocation in bolt_entry",
                        "sampler_sha256": sha256_file(args.sampler)}
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf8")
