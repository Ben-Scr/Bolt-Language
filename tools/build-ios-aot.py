#!/usr/bin/env python3
"""Export an executable Bolt project as a static iOS script XCFramework."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

from apple_xcframework import SLICES, check_archive, create_xcframework

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {('arm64','iphoneos'):'ios-arm64', ('arm64','iphonesimulator'):'ios-sim-arm64',
           ('x86_64','iphonesimulator'):'ios-sim-x64'}


def build_slice(a, folder, arch, sdk):
    folder.mkdir()
    unit = folder/'Script.cpp'
    obj = folder/'Script.o'
    archive = folder/('lib'+a.module+'.a')
    subprocess.run([a.bolt, 'emit', a.project, '--release', '--target', TARGETS[arch,sdk], '-o', unit], check=True)
    flags = ['-std=c++20', '-O3', '-DNDEBUG', '-fPIC', '-fvisibility=hidden', '-I', ROOT/'include']
    flags += ['-DBOLT_MOBILE_ENTRY='+a.symbol, '-Dbolt_generated=bolt_generated_'+a.symbol,
              '-DBOLT_IOS_AOT_EXTERNAL_REGISTRY=1']
    project_root = a.project if a.project.is_dir() else a.project.parent
    for directory in [project_root, *a.header_dir]:
        flags += ['-I', directory]
    simulator = sdk == 'iphonesimulator'
    if a.zig:
        target = ('aarch64' if arch == 'arm64' else arch) + '-ios.' + a.minimum + ('-simulator' if simulator else '')
        lib = Path(json.loads(subprocess.check_output([a.zig, 'env'], text=True))['lib_dir']).resolve()
        compiler = [a.zig, 'c++', '-target', target, '-idirafter', lib/'libc/include/any-macos-any']
        if arch == 'arm64' and simulator:
            compiler.append('-mcpu=apple_m1')
        librarian = [a.zig, 'ar', 'rcs', archive, obj]
    else:
        target = arch + '-apple-ios' + a.minimum + ('-simulator' if simulator else '')
        compiler = ['xcrun', '--sdk', sdk, 'clang++', '-target', target]
        librarian = ['xcrun', '--sdk', sdk, 'libtool', '-static', '-o', archive, obj]
    subprocess.run([*compiler, *flags, '-c', unit, '-o', obj], check=True)
    subprocess.run(librarian, check=True)
    return archive


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('project',type=Path,help='Bolt source file or executable project directory')
    p.add_argument('--bolt',default='bolt',help='Installed desktop Bolt CLI')
    p.add_argument('--header-dir',action='append',type=Path,default=[],help='Additional native header directory; repeatable')
    p.add_argument('--symbol', default='bolt_mobile_run', help='C entry point, unique per script module')
    p.add_argument('--output',type=Path,default=Path('BoltScript-iOS.zip'))
    p.add_argument('--minimum',default='15.0')
    p.add_argument('--zig',type=Path,help='Cross-build using Zig 0.14.1 in place of Xcode')
    a=p.parse_args()
    if not re.fullmatch(r'[0-9]{1,3}\.[0-9]{1,2}(\.[0-9]{1,2})?',a.minimum):
        p.error('Use a numeric iOS deployment target such as 15.0')
    if not (ROOT/'include/bolt/MobileAot.h').is_file():
        p.error('Run this tool from the extracted Bolt iOS SDK or source tree')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', a.symbol, flags=re.ASCII):
        p.error('--symbol requires a C identifier starting with a letter')
    a.module = 'BoltScript' if a.symbol == 'bolt_mobile_run' else 'BoltScript_'+a.symbol
    a.project=a.project.resolve()
    if a.zig:
        a.zig=a.zig.resolve()
    output=a.output.resolve()
    if output.suffix.lower()!='.zip':
        p.error('--output must name a .zip file')
    output.parent.mkdir(parents=True,exist_ok=True)
    version=[int(v) for v in a.minimum.split('.')]+[0]
    packed=(version[0]<<16)|(version[1]<<8)|version[2]
    with tempfile.TemporaryDirectory(prefix='bolt-script-ios-',dir=output.parent) as private:
        work=Path(private)
        headers=work/'Headers'
        headers.mkdir()
        if a.symbol == 'bolt_mobile_run':
            (headers/'bolt').mkdir()
            shutil.copy2(ROOT/'include/bolt/MobileAot.h',headers/'bolt')
            declaration = 'bolt/MobileAot.h'
        else:
            declaration = a.module+'.h'
            (headers/declaration).write_text(
                '#pragma once\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
                f'int {a.symbol}(int argc, const char* const* argv);\n'
                '#ifdef __cplusplus\n}\n#endif\n')
        (headers/'module.modulemap').write_text(
            f'module {a.module} {{\n  header "{declaration}"\n  link "c++"\n  export *\n}}\n')
        libraries={}
        for arch,sdk in SLICES:
            archive=build_slice(a,work/(sdk+'-'+arch),arch,sdk)
            check_archive(archive,arch,sdk,packed)
            libraries[arch,sdk]=archive
        framework=work/(a.module+'.xcframework')
        create_xcframework(framework,headers,libraries,xcode=not a.zig)
        temporary=work/'package.zip'
        with zipfile.ZipFile(temporary,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as result:
            for path in sorted(framework.rglob('*')):
                if path.is_file():
                    result.write(path,path.relative_to(work))
        temporary.replace(output)
    print(f'Packaged iOS/ARM64 and ARM64/x86-64 simulator scripts: {output}')


if __name__=='__main__':
    main()
