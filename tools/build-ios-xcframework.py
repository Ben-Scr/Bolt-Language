#!/usr/bin/env python3
"""Build BoltInterpreter.xcframework for physical iPhones/iPads and both simulator CPUs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

from apple_xcframework import SLICES, check_archive, create_xcframework

ROOT = Path(__file__).resolve().parents[1]
MODULE = 'module BoltInterpreter {\n    header "bolt/InterpreterHost.h"\n    link "c++"\n    export *\n}\n'


def xcode_build(a, build):
    libraries = {}
    for arch, sdk in SLICES:
        folder = build / f'{sdk}-{arch}'
        sysroot = subprocess.check_output(['xcrun', '--sdk', sdk, '--show-sdk-path'], text=True).strip()
        subprocess.run(['cmake', '-S', str(ROOT/'ports/mobile'), '-B', str(folder), '-G', 'Xcode',
                        '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT='+sysroot, '-DCMAKE_OSX_ARCHITECTURES='+arch,
                        '-DCMAKE_OSX_DEPLOYMENT_TARGET='+a.minimum, '-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO',
                        '-DBOLT_INTERPRETER_TESTS=OFF'], check=True)
        subprocess.run(['cmake', '--build', str(folder), '--config', 'Release', '--target', 'BoltInterpreter',
                        '--parallel', str(a.jobs)], check=True)
        archives = list(folder.rglob('libBoltInterpreter.a'))
        if len(archives) != 1:
            raise ValueError(f'Expected one Release archive in {folder}')
        libraries[arch,sdk] = archives[0]
    return libraries


def zig_build(a, build, zig):
    lib = Path(json.loads(subprocess.check_output([zig, 'env'], text=True))['lib_dir']).resolve()
    cmake = (ROOT/'ports/mobile/CMakeLists.txt').read_text()
    sources = re.findall(r'"\$\{BOLT_ROOT\}/(src/[^"]+\.cpp)"', cmake)
    if not sources:
        raise ValueError('Cannot read the mobile interpreter sources')
    libraries = {}
    for arch, sdk in SLICES:
        target = ('aarch64' if arch == 'arm64' else arch) + '-ios.' + a.minimum
        if sdk == 'iphonesimulator':
            target += '-simulator'
        folder = build / target
        folder.mkdir(parents=True, exist_ok=True)
        links = {'.bolt-source':ROOT, '.bolt-zig':zig, '.darwin-headers':lib/'libc/include/any-macos-any'}
        for name, destination in links.items():
            path = folder / name
            if not os.path.lexists(path):
                path.symlink_to(destination, target_is_directory=destination.is_dir())
            elif not path.is_symlink() or path.resolve() != destination:
                raise ValueError(f'Choose a new build directory for the changed toolchain: {path}')
        subprocess.run(['make', '-f', '.bolt-source/ports/mobile/AppleCross.mk', f'-j{a.jobs}',
                        'TARGET='+target, 'SOURCES='+' '.join(sources)], cwd=folder, check=True)
        libraries[arch,sdk] = folder / 'libBoltInterpreter.a'
    return libraries


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True, help='Path to the ZIP')
    p.add_argument('--minimum', default='15.0')
    p.add_argument('--zig', type=Path, help='Cross-build using Zig 0.14.1 on Linux/macOS')
    p.add_argument('-j', '--jobs', type=int, default=2)
    a = p.parse_args()
    if not re.fullmatch(r'[0-9]{1,3}\.[0-9]{1,2}(\.[0-9]{1,2})?', a.minimum) or a.jobs < 1:
        p.error('Choose a valid minimum iOS version and a positive job count')
    if not a.zig and sys.platform != 'darwin':
        p.error('A native Xcode build requires macOS; use --zig for Darwin object cross-compilation')
    minimum = [int(v) for v in a.minimum.split('.')] + [0]
    packed = (minimum[0] << 16) | (minimum[1] << 8) | minimum[2]
    build = a.build.resolve()
    if build == ROOT:
        p.error('Choose a separate build directory')
    build.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT/'tools/embed-stdlib.py')], check=True)
    libraries = zig_build(a, build, a.zig.resolve()) if a.zig else xcode_build(a, build)
    for (arch,sdk), library in libraries.items():
        count=check_archive(library,arch,sdk,packed)
        print(f'Verified {count} {arch} {sdk} objects targeting {a.minimum}')
    output = a.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='bolt-ios-', dir=output.parent) as temp:
        parent = Path(temp)
        package = parent/'Bolt-iOS'
        framework = package/'BoltInterpreter.xcframework'
        headers = parent/'Headers'
        (headers/'bolt').mkdir(parents=True)
        shutil.copy2(ROOT/'include/bolt/InterpreterHost.h', headers/'bolt')
        (headers/'module.modulemap').write_text(MODULE)
        create_xcframework(framework,headers,libraries,xcode=not a.zig)
        shutil.copytree(ROOT/'include',package/'include')
        (package/'tools').mkdir()
        for name in ('build-ios-aot.py','apple_xcframework.py'):
            shutil.copy2(ROOT/'tools'/name,package/'tools'/name)
        shutil.copytree(ROOT/'examples/iOSAot',package/'examples/iOSAot')
        shutil.copy2(ROOT/'docs/iOS-Interpreter.md',package/'README.md')
        shutil.copy2(ROOT/'LICENSE',package/'LICENSE')
        shutil.copytree(ROOT/'docs/licenses',package/'licenses')
        if a.zig:
            lib=Path(json.loads(subprocess.check_output([a.zig.resolve(),'env'],text=True))['lib_dir']).resolve()
            shutil.copy2(lib.parent/'LICENSE',package/'licenses/Zig.txt')
            shutil.copy2(lib/'libcxx/LICENSE.TXT',package/'licenses/libcxx.txt')
        records=sorted(p for p in package.rglob('*') if p.is_file())
        sums=package/'SHA256SUMS'
        sums.write_text(''.join(hashlib.sha256(f.read_bytes()).hexdigest()+'  '+f.relative_to(package).as_posix()+'\n' for f in records))
        temporary=parent/'package.zip'
        with zipfile.ZipFile(temporary,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
            for file in [*records,sums]:
                z.write(file,file.relative_to(parent))
        with zipfile.ZipFile(temporary) as z:
            if z.testzip() is not None:
                raise ValueError('Corrupt XCFramework package')
        temporary.replace(output)
    print(hashlib.sha256(output.read_bytes()).hexdigest(), output)


if __name__ == '__main__':
    main()
