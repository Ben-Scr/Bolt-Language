#!/usr/bin/env python3
"""Cross-build the portable Windows x64 SDK on Linux using LLVM-MinGW and vendor-provided Windows LLVM binaries."""
import argparse
import hashlib
from pathlib import Path
import stat
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def link_input(build, name, source):
    path = build / name
    if path.is_symlink() and path.resolve() == source:
        return
    if path.is_symlink() or path.exists():
        raise ValueError(f"{path} already references a different checkout or toolchain")
    path.symlink_to(source, target_is_directory=True)


TARGETS = {"x64": ("x86_64", 0x8664), "arm64": ("aarch64", 0xaa64)}


def check_machine(path, machine):
    with path.open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise ValueError(f"Expected a Windows executable: {path}")
        stream.seek(60)
        offset = stream.read(4)
        if len(offset) == 4:
            stream.seek(int.from_bytes(offset, "little"))
            if stream.read(6) == b"PE\0\0" + struct.pack("<H", machine):
                return
    raise ValueError(f"Windows binary does not match the requested CPU: {path}")


def compile_sdk(build, tools, jobs, arch="x64"):
    build.mkdir(parents=True, exist_ok=True)
    marker = build / ".bolt-target"
    if marker.is_file() and marker.read_text().strip() != arch:
        raise ValueError("Choose a separate build directory for each Windows architecture")
    if not marker.exists() and (build / "obj").exists() and arch != "x64":
        raise ValueError("Choose a clean object directory for the first Windows ARM64 build")
    marker.write_text(arch + "\n")
    link_input(build, ".bolt-source", ROOT)
    link_input(build, ".bolt-tools", tools)
    match = re.search(r"set\(BOLT_CORE_SOURCES\s+(.*?)\)", (ROOT/"CMakeLists.txt").read_text(), re.S)
    if not match:
        raise ValueError("BOLT_CORE_SOURCES is missing from CMakeLists.txt")
    sources = match.group(1).split()
    if not all(f.endswith(".cpp") and (ROOT/f).is_file() for f in sources):
        raise ValueError("Unexpected BOLT_CORE_SOURCES entry")
    subprocess.run([sys.executable, str(ROOT/"tools/embed-stdlib.py")], check=True)
    subprocess.run(["make", "-f", ".bolt-source/tools/WindowsSDK.mk", f"-j{jobs}",
                    "CORE_SOURCES="+" ".join(sources), "TARGET_CPU=" + TARGETS[arch][0], "all"], cwd=build, check=True)


def pack_sdk(binary_dir, tools, llvm, output, vsix=None, arch="x64", windows_linker=None):
    cpu, machine = TARGETS[arch]
    syslibs = tools / (cpu + "-w64-mingw32") / "lib"
    for name in ("bolt.exe", "BoltRuntime.dll", "BoltCompiler.dll"):
        check_machine(binary_dir / name, machine)
    runtime_files = {"LLVM-C.dll": llvm / "bin/LLVM-C.dll"}
    if windows_linker is None:
        runtime_files["lld-link.exe"] = llvm / "bin/lld-link.exe"
    else:
        # LLD selects the COFF driver from argv[0]. LLVM-MinGW ships its
        # multi-format Windows executable under the GNU driver name.
        linker_bin = windows_linker / "bin"
        runtime_files["lld-link.exe"] = linker_bin / "ld.lld.exe"
        for name in ("libLLVM-23.dll", "libc++.dll", "libunwind.dll"):
            runtime_files[name] = linker_bin / name
    for path in runtime_files.values():
        check_machine(path, machine)
    files = {f"bin/{name}": binary_dir/name for name in
             ("bolt.exe", "BoltCompiler.dll", "BoltCompiler.lib", "BoltRuntime.dll", "BoltRuntime.lib", "libBoltCore.a")}
    files.update({f"bin/{name}": path for name, path in runtime_files.items()})
    for name in ("kernel32", "bcrypt", "shell32", "ws2_32"):
        files[f"bin/windows-libs/{arch}/{name}.lib"] = syslibs/f"lib{name}.a"

    def add_tree(source, destination, ignored=()):
        if not source.is_dir():
            raise FileNotFoundError(source)
        for path in source.rglob("*"):
            relative = path.relative_to(source)
            if not any(part in ignored for part in relative.parts) and path.is_file():
                files[f"{destination}/{relative.as_posix()}"] = path

    add_tree(ROOT/"include", "bin/include")
    add_tree(ROOT/"stdlib", "share/bolt/stdlib")
    add_tree(ROOT/"examples/FunkeBolt", "examples/FunkeBolt", {"bin", ".bolt"})
    for name in ("Unicode", "SplitMix64", "FastFloat", "LLVM", "MinGW-w64"):
        files[f"share/bolt/licenses/{name}.txt"] = ROOT/"docs/licenses"/f"{name}.txt"
    files["share/bolt/licenses/LLVM-MinGW.txt"] = tools/"LICENSE.TXT"
    if windows_linker is not None:
        files["share/bolt/licenses/LLVM-MinGW-Windows-linker.txt"] = windows_linker/"LICENSE.TXT"
    files["share/bolt/LICENSE"] = ROOT/"LICENSE"
    files["bin/bolt-watch.py"] = ROOT/"tools/bolt-watch.py"
    files["README.md"] = ROOT/"docs/Windows-SDK.md"
    for name in ("setup.cmd", "uninstall.cmd", "sdk-setup.ps1", "SETUP.md"):
        files[name] = ROOT/"tools/sdk-setup"/name
    if vsix:
        files["Bolt-VSCode.vsix"] = vsix

    prefix = f"Bolt-Windows-{arch}/"
    def member(name, mode=0o644):
        info = zipfile.ZipInfo(prefix + name)
        info.create_system = 3
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = (stat.S_IFREG | mode) << 16
        return info

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name+".", delete=False) as handle:
        temp = Path(handle.name)
    try:
        manifest = []
        with zipfile.ZipFile(temp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for name in sorted(files, key=lambda path: Path(path).parts):
                path = files[name]
                digest = hashlib.sha256()
                mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
                with path.open("rb") as source, archive.open(member(name, mode), "w") as dest:
                    while block := source.read(1 << 20):
                        dest.write(block)
                        digest.update(block)
                manifest.append(f"{digest.hexdigest()}  {name}\n")
            archive.writestr(member("SHA256SUMS"), "".join(manifest).encode("utf-8"))
        with zipfile.ZipFile(temp) as archive:
            if archive.testzip() is not None:
                raise RuntimeError("Corrupt Windows SDK archive")
        temp.chmod(0o644)
        temp.replace(output)
    finally:
        temp.unlink(missing_ok=True)
    with output.open("rb") as published:
        checksum = hashlib.sha256()
        while block := published.read(1 << 20):
            checksum.update(block)
    print(output, checksum.hexdigest())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--arch", choices=TARGETS, default="x64", help="Windows CPU architecture")
    ap.add_argument("--mingw", type=Path, required=True, help="Linux-hosted LLVM-MinGW root")
    ap.add_argument("--llvm", type=Path, required=True, help="Extracted Windows LLVM release root")
    ap.add_argument("--build", type=Path, required=True, help="Directory for native objects and bin")
    ap.add_argument("--output", type=Path, required=True, help="Destination Windows SDK zip")
    ap.add_argument("--vsix", type=Path, help="Include a packaged Bolt VS Code extension in the SDK")
    ap.add_argument("-j", type=int, default=2)
    ap.add_argument("--windows-linker", type=Path, help="Use a native LLVM-MinGW 23 Windows distribution for LLD; --llvm still supplies the compiler backend.")
    args = ap.parse_args()
    build = args.build.resolve()
    if not sys.platform.startswith("linux") or args.j < 1 or build == ROOT:
        ap.error("Use a separate build directory on Linux and a positive number of jobs")
    tools, llvm = args.mingw.resolve(), args.llvm.resolve()
    for input_file in [tools/"bin"/(TARGETS[args.arch][0] + "-w64-mingw32-clang++"),llvm/"bin/LLVM-C.dll",
                       args.windows_linker/"bin/ld.lld.exe" if args.windows_linker else llvm/"bin/lld-link.exe"]:
        if not input_file.is_file():
            ap.error(f"Required input is missing: {input_file}")
    if args.vsix and not args.vsix.is_file():
        ap.error(f"VSIX file is missing: {args.vsix}")
    compile_sdk(build,tools,args.j,args.arch)
    pack_sdk(build/"bin",tools,llvm,args.output.resolve(), args.vsix, args.arch, args.windows_linker)


if __name__ == "__main__":
    main()
