#!/usr/bin/env python3
"""Cross-link a macOS CLI, host or C ABI dylib with LLVM LLD."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--linker',required=True,type=Path)
    p.add_argument('--zig',required=True,type=Path)
    p.add_argument('--arch',required=True,choices=('x86_64','aarch64','arm64'))
    p.add_argument('--minimum',required=True)
    p.add_argument('--role',required=True,choices=('Compiler','Runtime','CLI','Editor'))
    p.add_argument('--cxx-libs',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('objects',nargs='+',type=Path)
    args=p.parse_args()
    arch='arm64' if args.arch=='aarch64' else args.arch
    linker=args.linker.resolve()
    cmd=[str(linker)]
    # A generic LLD binary has multiple drivers. A dedicated ld64.lld
    # entry point selects its flavor through argv[0].
    if linker.name.startswith('lld'):
        cmd+=['-flavor','darwin']
    lib=Path(json.loads(subprocess.check_output([args.zig.resolve(),'env'],text=True))['lib_dir']).resolve()
    system=lib/'libc/darwin/libSystem.tbd'
    sdk=json.loads((lib/'libc/darwin/SDKSettings.json').read_text())['MinimalDisplayName']
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent,prefix='bolt-link-') as temp:
        result=Path(temp)/args.output.name
        cmd+=['-arch',arch,'-platform_version','macos',args.minimum,sdk,'-dead_strip']
        if args.role in ('Compiler','Runtime'):
            cmd+=['-dylib','-install_name','@rpath/'+args.output.name,'-compatibility_version','1.0',
                  '-current_version','1.0','-exported_symbols_list',ROOT/'src/native'/('Mac'+args.role+'.exports')]
        cmd+=['-o',result,*args.objects]
        cmd+=[args.cxx_libs/name for name in ('libc++abi.a','libc++.a','libcompiler_rt.a')]
        cmd.append(system)
        subprocess.run(cmd,check=True)
        result.replace(args.output)


if __name__=='__main__':
    main()
