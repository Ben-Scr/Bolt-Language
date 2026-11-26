#!/usr/bin/env python3
"""Build and package Bolt for macOS using Xcode or a Zig Darwin cross toolchain."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

from apple_xcframework import check_archive

ROOT = Path(__file__).resolve().parents[1]
BINARIES = ('bolt', 'libBoltRuntime.dylib', 'libBoltCompiler.dylib', 'libBoltCore.a')
HELPERS = ('setup.sh', 'uninstall.sh', 'sdk-setup.py', 'SETUP.md')


def link_tool(path, target):
    if not os.path.lexists(path):
        path.symlink_to(target, target_is_directory=target.is_dir())
    elif not path.is_symlink() or path.resolve() != target:
        raise ValueError(f'{path} already refers to a different source tree or toolchain')


def compile_sdk(args, build):
    marker = build / '.bolt-macos-target.json'
    identity = {'arch': args.arch, 'minimum': args.minimum,
                'compiler': str(args.zig.resolve()) if args.zig else 'Xcode'}
    if marker.is_file() and json.loads(marker.read_text()) != identity:
        raise ValueError('Select a separate build directory for each architecture, minimum OS and compiler')
    if not marker.is_file():
        if any((build / item).exists() for item in ('obj', 'bin', 'CMakeCache.txt')):
            raise ValueError('Choose an empty build directory or use --package-only for verified binaries')
        marker.write_text(json.dumps(identity))
    subprocess.run([sys.executable, str(ROOT / 'tools/embed-stdlib.py')], check=True)
    if args.zig:
        link_tool(build / '.bolt-source', ROOT)
        link_tool(build / '.bolt-zig', args.zig.resolve())
        tool=args.darwin_linker or shutil.which('ld64.lld') or shutil.which('lld')
        if not tool:
            raise ValueError('A cross-build needs a host Mach-O linker: supply --darwin-linker /path/to/ld64.lld or lld')
        link_tool(build/'.bolt-darwin-linker',Path(tool).resolve())
        setting = re.search(r'set\(BOLT_CORE_SOURCES\s+(.*?)\)', (ROOT / 'CMakeLists.txt').read_text(), re.S)
        sources = setting.group(1).split() if setting else []
        if not sources or any(not p.endswith('.cpp') or not (ROOT / p).is_file() for p in sources):
            raise ValueError('Cannot read BOLT_CORE_SOURCES from CMakeLists.txt')
        arch = 'aarch64' if args.arch == 'arm64' else args.arch
        subprocess.run(['make', '-f', '.bolt-source/tools/MacOSSDK.mk', f'-j{args.jobs}',
                        f'BOLT_ZIG_ARCH={arch}', f'BOLT_DEPLOYMENT={args.minimum}',
                        'CORE_SOURCES=' + ' '.join(sources)], cwd=build, check=True)
    else:
        subprocess.run(['cmake', '-S', str(ROOT), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release',
                        f'-DCMAKE_OSX_ARCHITECTURES={args.arch}', f'-DCMAKE_OSX_DEPLOYMENT_TARGET={args.minimum}',
                        '-DBOLT_BUILD_TESTS=OFF', '-DBOLT_BUILD_EMBEDDING=OFF'], check=True)
        subprocess.run(['cmake', '--build', str(build), '--config', 'Release', '--parallel', str(args.jobs)], check=True)


def check_macho(path, arch, kind, minimum, allow_older=False):
    def compatible(version):
        return 0 < version <= minimum if allow_older else version == minimum

    data = path.read_bytes()
    cpu = {'arm64': 0x0100000C, 'x86_64': 0x01000007}[arch]
    if len(data) < 32 or struct.unpack_from('<IIII', data)[:2] != (0xFEEDFACF, cpu) or struct.unpack_from('<I', data, 12)[0] != kind:
        raise ValueError(f'{path} is not a thin macOS {arch} binary of the expected kind')
    count, size = struct.unpack_from('<II', data, 16)
    at = 32
    if at + size > len(data):
        raise ValueError(f'Invalid Mach-O command table in {path}')
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, at)
        if length < 8 or at + length > 32 + size:
            raise ValueError(f'Invalid Mach-O load command in {path}')
        if command == 0x32 and length >= 24:
            platform, target = struct.unpack_from('<II', data, at + 8)
            if platform == 1 and compatible(target):
                return
        if command == 0x24 and length >= 16 and compatible(struct.unpack_from('<I', data, at + 8)[0]):
            return
        at += length
    raise ValueError(f'{path} does not declare the requested macOS deployment target')


def pack(args, build):
    parts = [int(v) for v in args.minimum.split('.')]
    minimum = (parts[0] << 16) | (parts[1] << 8) | (parts[2] if len(parts) == 3 else 0)
    binaries = build / 'bin'
    for name, kind in ((BINARIES[0], 2), (BINARIES[1], 6), (BINARIES[2], 6)):
        check_macho(binaries / name, args.arch, kind, minimum)
    check_archive(binaries/BINARIES[3],args.arch,'macosx',minimum)
    other_arch = 'arm64' if args.arch == 'x86_64' else 'x86_64'
    if args.editor_dir:
        check_macho(args.editor_dir/'BoltEditorSim',args.arch,2,minimum)
    if args.other_sdk:
        check_macho(args.other_sdk/'libBoltRuntime.dylib', other_arch, 6, minimum)
        check_archive(args.other_sdk/'libBoltCore.a',other_arch,'macosx',minimum)
    for folder,arch in [(binaries,args.arch)]+([(args.other_sdk,other_arch)] if args.other_sdk else []):
        aot=folder/'aot'
        if aot.is_dir():
            for name in ('libc++.a','libc++abi.a','libcompiler_rt.a'):
                check_archive(aot/name,arch,'macosx',minimum,allow_older=True)
    if args.llvm_dir:
        llvm = args.llvm_dir
        check_macho(llvm/'bin/ld64.lld', args.arch, 2, minimum, allow_older=True)
        required = ('libLLVM.19.1.dylib','libc++.1.dylib','libz.1.dylib','libzstd.1.dylib',
                    'libxml2.16.dylib','libiconv.2.dylib','liblzma.5.dylib')
        for name in required:
            check_macho(llvm/'lib'/name,args.arch,6,minimum,allow_older=True)
        for name in ('sysroot/usr/lib/libSystem.tbd','sysroot/SDKSettings.json','share/provenance.json',
                     'share/sources/libiconv/libiconv-1.18.tar.gz'):
            if not (llvm/name).is_file():
                raise ValueError(f'Incomplete LLVM toolchain: {llvm/name}')
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='bolt-mac-package-', dir=output.parent) as directory:
        temp = Path(directory)
        root = temp / ('Bolt-macOS-' + ('x64' if args.arch == 'x86_64' else 'arm64'))
        dest = root / 'bin'
        dest.mkdir(parents=True)
        for name in BINARIES:
            shutil.copy2(binaries / name, dest / name)
        if args.other_sdk:
            foreign = dest / 'targets' / other_arch
            foreign.mkdir(parents=True)
            for name in ('libBoltRuntime.dylib','libBoltCore.a'):
                shutil.copy2(args.other_sdk/name, foreign/name)
            if (args.other_sdk/'aot').is_dir():
                shutil.copytree(args.other_sdk/'aot',foreign/'aot')
        if (binaries/'aot').is_dir():
            shutil.copytree(binaries/'aot',dest/'aot')
        if args.llvm_dir:
            shutil.copytree(args.llvm_dir, dest / 'llvm')
        shutil.copytree(ROOT / 'include', dest / 'include')
        shutil.copy2(ROOT / 'tools/bolt-watch.py', dest / 'bolt-watch.py')
        share = root / 'share/bolt'
        shutil.copytree(ROOT / 'stdlib', share / 'stdlib')
        licenses = share / 'licenses'
        shutil.copytree(ROOT / 'docs/licenses', licenses)
        if args.zig:
            lib = Path(json.loads(subprocess.check_output([str(args.zig.resolve()), 'env'], text=True))['lib_dir']).resolve()
            shutil.copy2(lib.parent / 'LICENSE', licenses / 'Zig.txt')
            for name in ('libcxx', 'libcxxabi', 'libunwind'):
                shutil.copy2(lib / name / 'LICENSE.TXT', licenses / (name + '.txt'))
        shutil.copy2(ROOT / 'LICENSE', share / 'LICENSE')
        shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', share / 'THIRD_PARTY_NOTICES.md')
        shutil.copy2(ROOT / 'docs/macOS-SDK.md', root / 'README.md')
        shutil.copytree(ROOT / 'examples/FunkeBolt', root / 'examples/FunkeBolt',
                        ignore=shutil.ignore_patterns('.bolt', 'bin'))
        if args.editor_dir:
            example=root/'examples/FunkeEditorBench'
            shutil.copytree(ROOT/'examples/FunkeEditorBench',example,
                            ignore=shutil.ignore_patterns('__pycache__','bin','obj','.bolt'))
            tools=example/'bin'/('macos-x64' if args.arch=='x86_64' else 'macos-arm64')
            tools.mkdir(parents=True)
            for name in ('BoltEditorSim','FunkeEditorBridge.dll','FunkeEditorBridge.runtimeconfig.json','FunkeEditorBridge.deps.json'):
                shutil.copy2(args.editor_dir/name,tools/name)
        for name in HELPERS:
            shutil.copy2(ROOT / 'tools/sdk-setup' / name, root / name)
        if args.vsix:
            shutil.copy2(args.vsix, root / 'Bolt-VSCode.vsix')
        files = sorted(p for p in root.rglob('*') if p.is_file())
        sums = root / 'SHA256SUMS'
        sums.write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' +
                                p.relative_to(root).as_posix() + '\n' for p in files), encoding='utf-8')
        archive = temp / 'sdk.zip'
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for path in [*files, sums]:
                z.write(path, path.relative_to(temp))
        with zipfile.ZipFile(archive) as z:
            if z.testzip() is not None:
                raise ValueError('Corrupt macOS SDK archive')
        archive.replace(output)
    print(hashlib.sha256(output.read_bytes()).hexdigest(), output)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--arch', choices=('arm64', 'x86_64'), required=True)
    p.add_argument('--minimum', default='13.0', help='Minimum macOS major.minor[.patch] version')
    p.add_argument('--zig', type=Path, help='Path to Zig 0.14.1 for a cross-build')
    p.add_argument('--editor-dir', type=Path, help='Include a built editor simulator with its .NET bridge and fixtures')
    p.add_argument('--other-sdk', type=Path, help='Include the other macOS SDK bin directory for cross-compilation')
    p.add_argument('--llvm-dir', type=Path, help='Bundle the toolchain staged by fetch-macos-llvm.py')
    p.add_argument('--darwin-linker', type=Path, help='Host LLVM LLD with Mach-O support for cross-building C ABI libraries')
    p.add_argument('--vsix', type=Path, help='Include a packaged VS Code extension')
    p.add_argument('--package-only', action='store_true', help='Package existing build/bin binaries')
    p.add_argument('-j', '--jobs', type=int, default=2)
    a = p.parse_args()
    if not re.fullmatch(r'[0-9]{1,3}\.[0-9]{1,2}(\.[0-9]{1,2})?', a.minimum) or a.jobs < 1:
        p.error('Choose a valid minimum macOS version and a positive job count')
    if not a.package_only and not a.zig and sys.platform != 'darwin':
        p.error('Supply --zig for a cross-build, or build natively on macOS')
    build = a.build.resolve()
    if build == ROOT:
        p.error('Choose a separate build directory')
    build.mkdir(parents=True, exist_ok=True)
    if not a.package_only:
        compile_sdk(a, build)
    pack(a, build)


if __name__ == '__main__':
    main()
