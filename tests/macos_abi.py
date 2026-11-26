#!/usr/bin/env python3
"""Link and execute Clang/Bolt mixed calls with both direct backends on macOS."""
import argparse
from pathlib import Path
import platform
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--emitter', required=True, type=Path)
args = p.parse_args()
arch = platform.machine()
if platform.system() != 'Darwin' or arch not in ('arm64', 'x86_64'):
    raise SystemExit('Run on an ARM64 or Intel macOS host')
source = Path(__file__).with_name('macos_abi_probe.c')
with tempfile.TemporaryDirectory(prefix='Bolt macOS ABI ') as td:
    root = Path(td)
    for mode, suffix in (('fast', '.o'), ('ir', '.ll')):
        obj = root / (mode + suffix)
        subprocess.run([args.emitter, arch, mode, obj], check=True)
        exe = root / mode
        subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-O2', '-arch', arch,
                        '-mmacosx-version-min=13.0', str(source), str(obj), '-o', str(exe)], check=True)
        subprocess.run([exe], check=True)
        print(f'PASS Mach-O {arch} {mode}: C ABI both directions, signed short, packed stack, FP, data addend')
