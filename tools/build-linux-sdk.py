#!/usr/bin/env python3
"""Build a Linux/glibc x86-64, ARM64 or RV64 SDK with GNU Make and GCC."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path, help="Directory for object files and the bin/ SDK")
    parser.add_argument("-j", "--jobs", type=int, default=2, help="Parallel compiler jobs (default 2)")
    parser.add_argument("--cross-prefix", default="",
                        help="GCC tool prefix, e.g. aarch64-linux-gnu- (default: native CC, CXX, AR)")
    parser.add_argument("--dry-run", action="store_true", help="Show the build commands")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Run this SDK build on Linux with a glibc target toolchain.")
    if args.cross_prefix:
        cc, cxx, ar = (args.cross_prefix + tool for tool in ("gcc", "g++", "ar"))
    else:
        cc, cxx, ar = (os.environ.get(key, default) for key, default in
                       (("CC", "cc"), ("CXX", "g++"), ("AR", "ar")))
    def compiler_target(tool):
        return subprocess.check_output([tool, "-dumpmachine"], text=True).strip()
    triple = compiler_target(cxx)
    arch = triple.partition("-")[0]
    if arch not in ("x86_64", "aarch64", "riscv64") or not re.fullmatch(r"(?:x86_64|aarch64|riscv64)-.*linux-gnu", triple):
        parser.error(f"Unsupported SDK target {triple}: use x86_64-linux-gnu, aarch64-linux-gnu or riscv64-linux-gnu")
    if compiler_target(cc).partition("-")[0] != arch:
        parser.error("The C and C++ toolchains target different architectures")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build = args.build_dir.resolve()
    if build == ROOT:
        parser.error("Select a separate directory for generated files")
    build.mkdir(parents=True, exist_ok=True)
    target_file = build / ".bolt-sdk-target"
    if target_file.is_file() and target_file.read_text().strip() != triple:
        parser.error("Choose a different build directory for a different target")
    # Older builds have no target marker. Reject objects from another architecture.
    for existing in (build / "bin/bolt", build / "obj/src/Main.o"):
        if existing.is_file():
            with existing.open("rb") as f:
                header = f.read(20)
            machine = {"aarch64": 183, "x86_64": 62, "riscv64": 243}[arch]
            if header[:4] != b"\x7fELF" or int.from_bytes(header[18:20], "little") != machine:
                parser.error(f"{existing} belongs to a different architecture; select a new build directory")
    if not args.dry_run and not target_file.exists():
        target_file.write_text(triple + "\n")
    # A stable relative source path lets the generated make dependencies work
    # with spaces in either the checkout or the build directory.
    checkout = build / ".bolt-source"
    if not os.path.lexists(checkout):
        checkout.symlink_to(ROOT, target_is_directory=True)
    elif not checkout.is_symlink() or checkout.resolve() != ROOT:
        parser.error(f"{checkout} already points to a different source tree")
    project = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    sources = re.search(r"set\(BOLT_CORE_SOURCES\s+(.*?)\)", project, re.DOTALL)
    if sources is None:
        parser.error("Cannot read BOLT_CORE_SOURCES from CMakeLists.txt")
    core = sources.group(1).split()
    if not all(name.endswith(".cpp") and (ROOT / name).is_file() for name in core):
        parser.error("Unsupported entry in BOLT_CORE_SOURCES")
    if not args.dry_run:
        subprocess.run([sys.executable, str(ROOT / "tools/embed-stdlib.py")], check=True)
    make = ["make", "-f", ".bolt-source/tools/LinuxSDK.mk", f"-j{args.jobs}"]
    if args.dry_run:
        make.append("-n")
    subprocess.run(make + [f"CC={cc}", f"CXX={cxx}", f"AR={ar}", f"BOLT_ARCH={arch}",
                           "CORE_SOURCES=" + " ".join(core), "all"], cwd=build, check=True)
    if not args.dry_run:
        output = build / "bin"
        shutil.copytree(ROOT / "include", output / "include", dirs_exist_ok=True)
        watcher = output / "bolt-watch.py"
        shutil.copy2(ROOT / "tools/bolt-watch.py", watcher)
        watcher.chmod(0o755)
        print(f"SDK ready in {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
