#!/usr/bin/env python3
"""Build and verify individually timed Bolt primitive kernels."""
import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import operator
import os
from pathlib import Path
import random
import statistics
import struct
import subprocess
import tempfile

from compare import executable_path, sha256_file
from suite import cpu_info


INTS = ("byte", "short", "ushort", "int", "uint", "long", "ulong")
FLOATS = ("float", "double")
UNSIGNED = ("byte", "ushort", "uint", "ulong", "char")
WIDTH = 1024
FLAGS = {"fast": ["--debug", "--no-debug-info", "--backend", "fast"],
         "llvm": ["--release", "--backend", "llvm"], "cpp": ["--release", "--backend", "cpp"],
         "fast-jit": ["--jit", "--debug", "--backend", "fast", "--steps", "0"],
         "llvm-jit": ["--jit", "--release", "--backend", "llvm", "--steps", "0"],
         "interpreter": ["--runtime", "--steps", "0"]}


@dataclass
class Kernel:
    name: str
    setup: str
    loop: str
    output: str
    expected: int
    readback: str = ""


def input_words(seed):
    words = []
    state = seed
    for _ in range(WIDTH):
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        words.append(state & 65535)
    return words


def arithmetic_cases(ty):
    fp = ty in FLOATS
    for name, op, apply in [
        ("add", "+", operator.add), ("sub", "-", operator.sub), ("mul", "*", operator.mul),
        ("div", "/", operator.truediv if fp else lambda a, b: int(a / b)),
        ("eq", "==", operator.eq), ("ne", "!=", operator.ne), ("lt", "<", operator.lt),
        ("le", "<=", operator.le), ("gt", ">", operator.gt), ("ge", ">=", operator.ge),
    ]:
        compare = name in ("eq", "ne", "lt", "le", "gt", "ge")
        yield name, f"(a {op} b)" + (" ? 1 : 0" if compare else ""), apply, fp and not compare
    if ty not in UNSIGNED:
        yield "neg", "-a", lambda a, b: -a, fp
    if not fp:
        for name, expr, apply in [
            ("rem", "a % b", lambda a, b: a - int(a / b) * b),
            ("and", "a & b", operator.and_), ("or", "a | b", operator.or_), ("xor", "a ^ b", operator.xor),
            ("not_low_byte", f"(~a) & ({ty})255", lambda a, b: (~a) & 255),
            ("shl", "a << ((int)b & 7)", lambda a, b: a << (b & 7)),
            ("shr", "a >> ((int)b & 7)", lambda a, b: a >> (b & 7)),
        ]:
            yield name, expr, apply, False


def scalar(ty, name, expr, apply, fp_result, words, passes, portable, cast=False):
    # Setup is outside the clock. Every pass visits every input exactly once.
    bias = 16 if name == "sub" and ty in UNSIGNED else 0 if cast or ty in UNSIGNED else -32
    fraction = 0.5 if cast and ty in FLOATS else 0
    lhs = f"((w & 63) + ({bias}))" + (" + 0.5" if fraction else "")
    rhs = "(1 << ((w >> 8) & 3))" if ty in FLOATS else "(((w >> 8) & 15) + 1)"
    setup = f"""
        {ty}[] left = new {ty}[1024];
        {ty}[] right = new {ty}[1024];
        for (int j = 0; j < 1024; j++) {{
            int w = words[j];
            left[j] = ({ty})({lhs});
            right[j] = ({ty}){rhs};
        }}
        {"double" if fp_result else "long"} sum = 0;
    """
    x, y = "left", "right"
    if not portable:
        setup += f"{ty}* x = &left[0]; {ty}* y = &right[0];"
        x, y = "x", "y"
    loop = f"""
        for (int i = 0; i < count; i++) {{
            int j = (i ^ (i >> 10)) & 1023;
            {ty} a = {x}[j];
            {"" if cast or name in ("neg", "not_low_byte") else f"{ty} b = {y}[j];"}
            sum += ({"double" if fp_result else "long"})({expr});
        }}
    """
    total = 0
    for w in words:
        a = (w & 63) + bias + fraction
        b = 1 << ((w >> 8) & 3) if ty in FLOATS else ((w >> 8) & 15) + 1
        total += apply(a, b)
    total *= passes
    result = "Reinterpret<ulong>(sum)" if fp_result else "sum"
    expected = struct.unpack("<Q", struct.pack("<d", total))[0] if fp_result else int(total)
    return Kernel(f"{ty}.{name}", setup, loop, result, expected)


def memory_cases(words, passes):
    setup = "long sum = 0;"
    expected = sum(words) * passes
    inner = "sum += words[i];"
    for name, loop in [
        ("for", f"for (int i=0; i<1024; i++) {inner}"),
        ("while", f"int i=0; while (i<1024) {{ {inner} i++; }}"),
        ("prefix_inc", "int i=-1; while(i<1023) sum+=words[++i];"),
        ("postfix_inc", "int i=0; while(i<1024) sum+=words[i++];"),
        ("prefix_dec", "int i=1024; while(i>0) sum+=words[--i];"),
        ("postfix_dec", "int i=1023; while(i>=0) sum+=words[i--];"),
        ("foreach", "for (int value : words) sum += value;"),
        ("local_copy", "for (int i=0; i<1024; i++) { int v=words[i]; int copy=v; sum+=copy; }"),
        ("pointer_read", "for (int i=0; i<1024; i++) sum+=ptr[i];"),
    ]:
        yield Kernel(f"loop.{name}", setup + (" int* ptr=&words[0];" if name=="pointer_read" else ""),
                     f"for (int pass=0; pass<count/1024; pass++) {{ {loop} }}", "sum", expected)
    for name, access, extra in [
        ("array", "dst", "int[] dst=new int[1024];"),
        ("pointer", "dst", "int[] backing=new int[1024]; int* dst=&backing[0];"),
    ]:
        loop = f"""for (int i=0; i<count; i++) {{
            int j=i & 1023; int k=(j+17) & 1023;
            {access}[j]=words[j] ^ i; sum+={access}[k];
        }}"""
        dest = [0] * WIDTH
        total = 0
        for i in range(passes * WIDTH):
            j = i & 1023
            dest[j] = words[j] ^ i
            total += dest[(j + 17) & 1023]
        yield Kernel(f"access.{name}_write_read", setup + extra, loop, "sum", total)
    for size in (2, 12):
        fields = [f"F{i}" for i in range(size)]
        init = " ".join(f"data[j].{f}=words[j] + {n};" for n, f in enumerate(fields))
        extra = f"Rec{size}[] data=new Rec{size}[1024]; for(int j=0;j<1024;j++) {{ {init} }}"
        loop = """for (int i=0; i<count; i++) {
            int j=i & 1023; int k=(j+(i >> 10)+17) & 1023;
            data[j]=data[k]; data[j].F0 ^= j;
        }"""
        # All fields, including those untouched after assignment, are observable.
        checksum = " + ".join(f"data[j].{f}" for f in fields)
        data = [[w + n for n in range(size)] for w in words]
        for i in range(passes * WIDTH):
            j = i & 1023
            data[j] = data[(j + (i >> 10) + 17) & 1023].copy()
            data[j][0] ^= j
        yield Kernel(f"copy.struct_{size*4}B", setup + extra, loop, "sum", sum(map(sum, data)),
                     f"for (int j=0; j<1024; j++) sum += {checksum};")


def make_cases(words, passes, portable=False):
    for ty in (*INTS, *FLOATS):
        for name, expr, apply, fp_result in arithmetic_cases(ty):
            yield scalar(ty, name, expr, apply, fp_result, words, passes, portable)
        for dst in (*INTS, *FLOATS, "char"):
            if dst == "char" and ty in FLOATS:
                continue
            yield scalar(ty, "to_" + dst, f"({dst})a", lambda a, b: float(a) if dst in FLOATS else int(a),
                         dst in FLOATS, words, passes, portable, cast=True)
    for name, expr, apply in [
        ("logical_and", "(a > 0 && b > 8) ? 1 : 0", lambda a,b: a > 0 and b > 8),
        ("logical_or", "(a > 0 || b > 8) ? 1 : 0", lambda a,b: a > 0 or b > 8),
        ("logical_not", "!(a > 0) ? 1 : 0", lambda a,b: a <= 0),
        ("select", "a > b ? a : b", max),
    ]:
        yield scalar("int", name, expr, apply, False, words, passes, portable)
    for case in memory_cases(words, passes):
        if not portable or "pointer" not in case.name:
            yield case
    for ty, init, value in [
        ("bool", "(words[j] & 1) != 0", lambda w: bool(w & 1)),
        ("char", "(char)(words[j] & 127)", lambda w: w & 127),
    ]:
        setup = f"{ty}[] data=new {ty}[1024]; for(int j=0;j<1024;j++) data[j]={init}; long sum=0;"
        for name, op, test in [("eq", "==", operator.eq), ("ne", "!=", operator.ne)]:
            loop = f"""for(int i=0;i<count;i++) {{
                int j=i & 1023; sum+=(data[j] {op} data[(j+1) & 1023]) ? 1 : 0;
            }}"""
            total = sum(test(value(w), value(words[(j+1) % WIDTH])) for j, w in enumerate(words))
            yield Kernel(f"{ty}.{name}", setup, loop, "sum", total*passes)
    setup = """double[] data=new double[1024]; long sum=0;
        for(int j=0;j<1024;j++) data[j]=(double)(words[j] & 65535)*281474976710656.0;"""
    loop = "for(int i=0;i<count;i++) sum+=(long)(((ulong)data[i & 1023]) >> 48);"
    yield Kernel("double.to_ulong_full_range", setup, loop, "sum", sum(words)*passes)


def source(cases):
    records = []
    for size in (2, 12):
        fields = " ".join(f"int F{i};" for i in range(size))
        records.append(f"struct Rec{size} {{ public: {fields} }}")
    methods = []
    for i, c in enumerate(cases):
        methods.append(f"""
            void Run{i}(int[]& words, int count) {{
                {c.setup}
                Stopwatch clock = Stopwatch.StartNew();
                {c.loop}
                clock.Stop();
                {c.readback}
                Print({c.output});
                Print(clock.ElapsedTicks);
            }}""")
    calls = "\n".join(f"Run{i}(words, count);" for i in range(len(cases)))
    return ("import System.Diagnostics\n" + "\n".join(records) + "\nstatic class Program { public:\n" +
            "\n".join(methods) + """
        void Main(string[] args) {
            uint state = uint.Parse(args[0]);
            int count = int.Parse(args[1]);
            int[] words = new int[1024];
            for (int j=0;j<1024;j++) {
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                words[j] = (int)(state & 65535u);
            }
    """ + calls + "\n} }\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", required=True)
    parser.add_argument("--before", help="matching baseline SDK")
    parser.add_argument("--backends", nargs="+", choices=FLAGS, default=["fast", "llvm"])
    parser.add_argument("--cases", nargs="+", help="run selected kernels by name, e.g. int.add loop.foreach")
    parser.add_argument("--runs", type=int, default=9)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--passes", type=int, default=256, help="1024 iterations per pass")
    parser.add_argument("--seed", type=int, default=731)
    parser.add_argument("--json", required=True, type=Path)
    args = parser.parse_args()
    if not (0 < args.seed < 2**32 and 0 < args.passes < 2**21 and args.runs > 0 and args.warmup >= 0):
        parser.error("invalid seed, pass count or repetition count")
    compilers = {"after": executable_path(args.bolt)}
    if args.before:
        compilers["before"] = executable_path(args.before)
    portable = "interpreter" in args.backends
    cases = list(make_cases(input_words(args.seed), args.passes, portable))
    if args.cases:
        unknown = set(args.cases) - {c.name for c in cases}
        if unknown:
            parser.error("unknown or unavailable kernels: " + ", ".join(sorted(unknown)))
        cases = [c for c in cases if c.name in args.cases]
    count = args.passes * WIDTH
    report = {"host": cpu_info(), "utc": datetime.now(timezone.utc).isoformat(), "iterations": count,
              "seed": args.seed, "warmup": args.warmup, "runs": args.runs, "portable_array_inputs": portable,
              "flags": {b: FLAGS[b] for b in args.backends}, "stopwatch_tick_ns": 100,
              "compilers": {k: sha256_file(p) for k,p in compilers.items()}, "programs": {},
              "cases": {c.name: {"expected": c.expected, "ticks": {}} for c in cases}}
    rng = random.Random(args.seed)
    for start in range(0, len(cases), 16):
        with tempfile.TemporaryDirectory(prefix="bolt-primitives-") as work:
            batch = cases[start:start+16]
            directory = Path(work)
            src = directory / "Kernels.bolt"
            src.write_text(source(batch), encoding="utf8")
            commands = {}
            for backend in args.backends:
                for variant, compiler in compilers.items():
                    label = f"{backend}-{variant}"
                    dest = directory / label
                    dest.mkdir()
                    if backend == "interpreter" or backend.endswith("-jit"):
                        commands[label] = [compiler, "run", src, *FLAGS[backend], "--"]
                        report["programs"][f"{start}/{label}"] = {"source_sha256": sha256_file(src)}
                        continue
                    exe = dest / ("kernels.exe" if os.name=="nt" else "kernels")
                    flags = FLAGS[backend] + ([] if backend == "cpp" else ["--require-native"])
                    subprocess.run([compiler, "compile", src, *flags, "-o", exe],
                                   cwd=dest, check=True, timeout=180, stdout=subprocess.DEVNULL)
                    commands[label] = [exe]
                    report["programs"][f"{start}/{label}"] = {"source_sha256": sha256_file(src),
                                                               "binary_sha256": sha256_file(exe), "bytes": exe.stat().st_size}
            for r in range(args.warmup + args.runs):
                for label in rng.sample(list(commands), len(commands)):
                    result = subprocess.run([*commands[label], str(args.seed), str(count)], cwd=directory / label,
                                            text=True, capture_output=True, timeout=180)
                    if result.returncode:
                        raise RuntimeError(f"{label} {batch[0].name}: {result.stderr[-2000:]}")
                    lines = result.stdout.splitlines()
                    if len(lines) != 2*len(batch):
                        raise ValueError(f"{label}: expected {2*len(batch)} output lines, got {lines!r}")
                    for i, case in enumerate(batch):
                        value, ticks = map(int, lines[2*i:2*i+2])
                        if value != case.expected or ticks <= 0:
                            raise ValueError(f"{label} {case.name}: {value} != {case.expected} or invalid ticks {ticks}")
                        if r >= args.warmup:
                            report["cases"][case.name]["ticks"].setdefault(label, []).append(ticks)
            print(f"Verified {start+len(batch)}/{len(cases)} kernels", flush=True)
    for entry in report["cases"].values():
        entry["median_ns_per_iteration"] = {k: round(statistics.median(v)*100/count, 4)
                                            for k,v in entry["ticks"].items()}
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")


if __name__ == "__main__":
    main()
