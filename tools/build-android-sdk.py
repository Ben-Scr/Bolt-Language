#!/usr/bin/env python3
"""Cross-build and package the Android embedding SDK using the official NDK."""
import argparse
import hashlib
import os
import re
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ABIS = {"arm64-v8a": (2, 183), "x86_64": (2, 62), "armeabi-v7a": (1, 40), "x86": (1, 3)}


def verify_smoke(executable, target):
    elf_class, cpu = target
    with executable.open("rb") as f:
        header = f.read(64)
    if header[:6] != b"\x7fELF" + bytes((elf_class, 1)) or struct.unpack_from("<H", header, 18)[0] != cpu:
        raise ValueError(f"Expected a matching little-endian ELF Android host: {executable}")


def build(args):
    toolchain = args.ndk / "build/cmake/android.toolchain.cmake"
    if not toolchain.is_file():
        raise ValueError(f"Android NDK toolchain missing: {toolchain}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent, prefix=".bolt-android-") as directory:
        sdk = Path(directory) / "Bolt-Android"
        (sdk / "include/bolt").mkdir(parents=True)
        (sdk / "examples").mkdir()
        (sdk / "cmake").mkdir()
        (sdk / "licenses").mkdir()
        shutil.copytree(ROOT / "include/bolt", sdk / "include/bolt", dirs_exist_ok=True)
        shutil.copy2(ROOT / "tests/interpreter_host.c", sdk / "examples")
        shutil.copytree(ROOT / "tests/portable", sdk / "test/portable")
        shutil.copy2(ROOT / "ports/android/BoltAndroidConfig.cmake", sdk / "cmake")
        shutil.copy2(ROOT / "docs/Android.md", sdk / "README.md")
        shutil.copy2(ROOT / "LICENSE", sdk / "licenses/Bolt.txt")
        for name in ("LLVM.txt", "Unicode.txt", "SplitMix64.txt", "FastFloat.txt"):
            shutil.copy2(ROOT / "docs/licenses" / name, sdk / "licenses" / name)
        for name, destination in (("NOTICE", "Android-NDK.txt"), ("NOTICE.toolchain", "Android-NDK-toolchain.txt")):
            notice = args.ndk / name
            if notice.is_file():
                shutil.copy2(notice, sdk / "licenses" / destination)
        shutil.copy2(ROOT / "tools/bolt-android-aot.py", sdk)
        shutil.copy2(ROOT / "tests/mobile_aot_host.c", sdk / "examples")
        shutil.copy2(ROOT / "tests/portable/mobile_aot.bolt", sdk / "examples")
        for abi in args.abi:
            build_dir = args.build_dir / abi
            subprocess.run([args.cmake,"-S",str(ROOT/"ports/mobile"),"-B",str(build_dir),
                            "-DCMAKE_TOOLCHAIN_FILE="+str(toolchain), "-DANDROID_ABI="+abi,
                            "-DANDROID_PLATFORM=android-29", "-DANDROID_STL=c++_static",
                            "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON", "-DBOLT_INTERPRETER_TESTS=ON",
                            "-DCMAKE_BUILD_TYPE=Release"],check=True)
            subprocess.run([args.cmake,"--build",str(build_dir),"--config","Release",
                            "--parallel",str(args.jobs)],check=True)
            # Multi-config CMake generators place artifacts under Release/.
            artifacts = build_dir if (build_dir/"BoltInterpreterSmoke").is_file() else build_dir/"Release"
            verify_smoke(artifacts/"BoltInterpreterSmoke", ABIS[abi])
            libs = sdk/"lib"/abi
            tests = sdk/"test"/abi
            libs.mkdir(parents=True)
            tests.mkdir(parents=True)
            shutil.copy2(artifacts/"libBoltInterpreter.a",libs)
            shutil.copy2(artifacts/"BoltInterpreterSmoke",tests/"bolt-smoke")
            cache = (build_dir/"CMakeCache.txt").read_text(encoding="utf-8")
            strip = re.search(r"^CMAKE_STRIP:[^=]+=(.+)$",cache,re.MULTILINE)
            if strip and Path(strip[1]).is_file():
                subprocess.run([strip[1],"--strip-debug",str(libs/"libBoltInterpreter.a")],check=True)
                subprocess.run([strip[1],"--strip-debug",str(tests/"bolt-smoke")],check=True)
        files = sorted(p for p in sdk.rglob("*") if p.is_file())
        sums = sdk/"SHA256SUMS"
        sums.write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest()+"  "+p.relative_to(sdk).as_posix()+"\n" for p in files),encoding="utf-8")
        archive = Path(directory)/"sdk.zip"
        with zipfile.ZipFile(archive,"w",zipfile.ZIP_DEFLATED) as z:
            for path in [*files, sums]:
                name = path.relative_to(sdk.parent).as_posix()
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                executable = path.name in ("bolt-smoke", "bolt-android-aot.py")
                info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with path.open("rb") as source, z.open(info, "w") as member:
                    shutil.copyfileobj(source, member, 1024 * 1024)
        with zipfile.ZipFile(archive) as z:
            if z.testzip():raise ValueError("Android SDK zip verification failed")
        os.replace(archive,args.output)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ndk",type=Path,default=os.getenv("ANDROID_NDK_HOME"))
    p.add_argument("--build-dir",type=Path,required=True)
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--cmake",default="cmake")
    p.add_argument("--abi",choices=ABIS,nargs="+",default=list(ABIS))
    p.add_argument("-j","--jobs",type=int,default=2)
    args=p.parse_args()
    if not args.ndk or args.jobs<1 or len(set(args.abi))!=len(args.abi):
        p.error("Select an NDK, a positive job count and distinct ABIs.")
    args.ndk=args.ndk.resolve()
    try:
        build(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        p.exit(1,f"build-android-sdk: {exc}\n")


if __name__=="__main__":
    main()
