#!/usr/bin/env python3
"""Build one Bolt project as a native static Android game module with the NDK."""
import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

TARGETS = {
    "arm64-v8a": ("android-arm64", "aarch64-linux-android"),
    "armeabi-v7a": ("android-armv7", "armv7a-linux-androideabi"),
    "x86_64": ("android-x64", "x86_64-linux-android"),
    "x86": ("android-x86", "i686-linux-android"),
}


def ndk_programs(ndk):
    family = {"Linux": "linux", "Darwin": "darwin", "Windows": "windows"}.get(platform.system())
    if family is None:
        raise ValueError("Choose a host supported by the Android NDK.")
    suffix = ".exe" if family == "windows" else ""
    for folder in sorted((ndk / "toolchains/llvm/prebuilt").glob(family + "-*")):
        compiler = folder / "bin" / ("clang++" + suffix)
        archiver = folder / "bin" / ("llvm-ar" + suffix)
        if compiler.is_file() and archiver.is_file():
            return compiler, archiver
    raise ValueError(f"No {family} C++ toolchain found in Android NDK {ndk}")


def build(args):
    sdk = args.sdk.resolve()
    if not (sdk / "include/bolt/MobileAot.h").is_file():
        raise ValueError(f"Expected the Bolt Android SDK with C++ headers under {sdk}")
    compiler, archiver = ndk_programs(args.ndk.resolve())
    source = args.source.resolve(strict=True)
    output = args.output.resolve()
    if output.suffix != ".a" or output == source or (output.exists() and not output.is_file()):
        raise ValueError("Choose a separate .a file for the native archive.")
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", args.symbol, flags=re.ASCII):
        raise ValueError("--symbol requires a C identifier starting with a letter.")
    output.parent.mkdir(parents=True, exist_ok=True)
    target, triple = TARGETS[args.abi]
    with tempfile.TemporaryDirectory(prefix=".bolt-android-aot-", dir=output.parent) as directory:
        stage = Path(directory)
        unit, obj, archive = stage / "program.cpp", stage / "program.o", stage / "program.a"
        subprocess.run([args.bolt, "emit", str(source), "--target", target, "--release",
                        "-o", str(unit)], check=True)
        subprocess.run([compiler, "--target=" + triple + "29", "-std=c++20", "-O3", "-DNDEBUG",
                        "-fPIC", "-fexceptions", "-ffunction-sections", "-fdata-sections",
                        "-DBOLT_MOBILE_ENTRY=" + args.symbol, "-Dbolt_generated=bolt_generated_" + args.symbol,
                        "-I", sdk / "include", "-c", unit, "-o", obj], check=True)
        subprocess.run([archiver, "rcs", archive, obj], check=True)
        os.replace(archive, output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Bolt executable project directory or main source file")
    parser.add_argument("--abi", choices=TARGETS, required=True)
    parser.add_argument("--sdk", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--ndk", type=Path, default=os.environ.get("ANDROID_NDK_HOME"))
    parser.add_argument("--bolt", default="bolt", help="Bolt 0.16 desktop compiler")
    parser.add_argument("--symbol", default="bolt_mobile_run", help="C entry point, unique per linked script module")
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    if args.ndk is None:
        parser.error("Set ANDROID_NDK_HOME or pass --ndk.")
    try:
        print(build(args))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"bolt-android-aot: {error}\n")


if __name__ == "__main__":
    main()
