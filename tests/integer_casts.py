"""Check numeric cast boundaries against an independent oracle."""
import argparse
from itertools import product
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory

TYPES = {"byte": (8, False), "short": (16, True), "ushort": (16, False),
         "int": (32, True), "uint": (32, False), "long": (64, True), "ulong": (64, False)}


def bounds(name):
    width, signed = TYPES[name]
    return (-(1 << (width - 1)) if signed else 0, (1 << (width - signed)) - 1)


def literal(n):
    if n == -(1 << 63):
        return "(-9223372036854775807l - 1l)"
    if n > (1 << 63) - 1:
        return f"{n}ul"
    return f"{n}l"


def float_cast_cases(methods, calls, expected):
    # Neighbours in the source format, including fractional values just below
    # a signed minimum. Construct inputs by bits so parsing never rounds them.
    for src, fmt, raw, word in (("float", "f", "I", "uint"), ("double", "d", "Q", "ulong")):
        def bits(x):
            return struct.unpack(">" + raw, struct.pack(">" + fmt, x))[0]

        for dst in [*TYPES, *(["float"] if src == "double" else [])]:
            method = src + "To" + dst
            result = f"Reinterpret<uint>(({dst})x)" if dst == "float" else f"({dst})x"
            methods.append(f"void {method}({src} x) {{ try {{ Print({result}); }} "
                           "catch(Exception& e) { Print(e.Message); } }")
            anchors = [-0.0, 0.0, -1.0, 1.0, -0.75, 0.75, -1.5, 1.5, math.inf, -math.inf, math.nan]
            if dst == "float":
                # Halfway from FLT_MAX to the first overflowing power of two.
                anchors += [2.0**128, 2.0**128 - 2.0**103, -(2.0**128 - 2.0**103),
                            2.0**-149, -(2.0**-150)]
            else:
                lo, hi = bounds(dst)
                anchors += [float(lo), float(lo-1), float(hi+1), 2.0**63, 2.0**64]
            mask = (1 << (8 * struct.calcsize(fmt))) - 1
            for pattern in sorted({(bits(v) + delta) & mask for v in anchors for delta in (-1, 0, 1)}):
                x = struct.unpack(">" + fmt, struct.pack(">" + raw, pattern))[0]
                suffix = "ul" if word == "ulong" else "u"
                calls.append(f"{method}(Reinterpret<{src}>({pattern}{suffix}));")
                if dst == "float":
                    try:
                        packed = struct.pack(">f", x)
                        valid = math.isfinite(struct.unpack(">f", packed)[0])
                    except OverflowError:
                        valid = False
                    expected.append(str(int.from_bytes(packed, "big")) if valid else "floating-point overflow")
                else:
                    expected.append(str(int(x)) if math.isfinite(x) and lo <= int(x) <= hi
                                    else "integer conversion out of range")


def fixture(fuzz, seed):
    rng = random.Random(seed)
    methods, calls, expected = [], [], []
    for n, (src, dst) in enumerate(product(TYPES, repeat=2)):
        a, b = bounds(src)
        lo, hi = bounds(dst)
        cases = sorted({x for x in (a, a+1, b-1, b, -1, 0, 1, lo-1, lo, lo+1, hi-1, hi, hi+1)
                        if a <= x <= b})
        if fuzz:
            cases += [rng.randint(a, b) for _ in range(fuzz)]
        methods.append(f"void Case{n}({src} value) {{ try {{ Print(({dst})value); }} "
                       "catch(Exception& e) { Print(e.Message); } }")
        for x in cases:
            calls.append(f"Case{n}(({src}){literal(x)});")
            expected.append(str(x) if lo <= x <= hi else "integer conversion out of range")
    for src in TYPES:
        method = src + "ToChar"
        methods.append(f"void {method}({src} x) {{ try {{ Print((uint)(char)x); }} "
                       "catch(Exception& e) { Print(e.Message); } }")
        lo, hi = bounds(src)
        for x in sorted({lo, hi, -1, 0, 0xd7ff, 0xd800, 0xdfff, 0xe000, 0x10ffff, 0x110000, 2**32}):
            if not lo <= x <= hi:
                continue
            calls.append(f"{method}(({src}){literal(x)});")
            if not 0 <= x < 2**32:
                expected.append("integer conversion out of range")
            elif 0xd800 <= x <= 0xdfff or x > 0x10ffff:
                expected.append("char conversion out of Unicode range")
            else:
                expected.append(str(x))
    float_cast_cases(methods, calls, expected)
    return ("import System\npublic static class Program { public:\n" + "\n".join(methods) +
            "\nvoid Main(){\n" + "\n".join(calls) + "\n} }\n"), expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bolt", type=Path)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--fast", action="store_true", help="Exercise the Fast encoder explicitly without debug symbols")
    parser.add_argument("--fuzz", type=int, default=0, help="Seeded random values per source/target pair")
    parser.add_argument("--seed", type=int, default=90210)
    parser.add_argument("--save-fixture", type=Path, help="Write the generated source for reproduction")
    args = parser.parse_args()
    if args.fuzz < 0:
        parser.error("--fuzz must be nonnegative")
    bolt = str(args.bolt.resolve())
    case, expected = fixture(args.fuzz, args.seed)
    if args.save_fixture:
        args.save_fixture.write_text(case, encoding="utf-8")
    profiles = [name for name, enabled in (("--release",args.release),("--debug",args.debug)) if enabled]
    if args.fast:
        profiles.append("fast")

    with TemporaryDirectory() as root:
        source = Path(root) / "Casts.bolt"
        source.write_text(case, encoding="utf-8")
        exe = str(Path(root) / ("casts.exe" if sys.platform == "win32" else "casts"))
        for profile in profiles or ["--release", "--debug"]:
            flags = ["--debug", "--no-debug-info", "--backend", "fast"] if profile == "fast" else [profile]
            subprocess.run([bolt, "compile", str(source), *flags, "--require-native", "-o", exe], check=True)
            got = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.splitlines()
            if got != expected:
                first = next((i for i,(a,b) in enumerate(zip(got, expected)) if a != b), min(len(got),len(expected)))
                raise AssertionError((profile, first, got[first:first+1], expected[first:first+1], len(got)))
            print(f"{profile}: {len(got)} checked conversion results matched (random values per pair={args.fuzz}, seed={args.seed})")


if __name__ == "__main__":
    main()
