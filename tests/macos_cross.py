#!/usr/bin/env python3
"""Check Mach-O cross-architecture builds through the public CLI."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def run(args, expect=0):
    p=subprocess.run(args,capture_output=True,text=True)
    if (expect==0 and p.returncode) or (expect!=0 and not p.returncode):
        raise RuntimeError(f'{args} returned {p.returncode}\n{p.stdout}{p.stderr}')
    return p.stdout+p.stderr


def check_image(path,cpu,kind):
    with path.open('rb') as f:
        magic,actual,_,file_type=struct.unpack('<IIII',f.read(16))
    assert (magic,actual,file_type)==(0xfeedfacf,cpu,kind),path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bolt',required=True,type=Path)
    p.add_argument('--runtime-dir',type=Path, help='Foreign BoltRuntime when using a one-architecture build tree')
    opts=p.parse_args()
    bolt=opts.bolt.resolve()
    host=json.loads(run([bolt,'version','--json']))['target']
    arch,cpu,conditional=('arm64',0x0100000c,'ARM64') if host.startswith('x86_64-') else ('x86_64',0x01000007,'X64')
    with tempfile.TemporaryDirectory(prefix='bolt Mac cross ') as temp:
        root=Path(temp)
        program=root/'Architecture.bolt'
        program.write_text('#if MacOS && '+conditional+'\npublic static class Program { public: int Main() { return 42; } }\n#else\nTHIS IS THE WRONG TARGET\n#endif\n')
        run([bolt,'check',program,'--target',arch])
        assert 'foreign macOS target' in run([bolt,'run',program,'--target',arch],1)
        emitted=root/'scene.cpp'
        run([bolt,'emit',program,'--target',arch,'-o',emitted])
        assert 'defined(__APPLE__)' in emitted.read_text()
        ir=root/'scene.ll'
        run([bolt,'emit-ir',program,'--target',arch,'-o',ir])
        assert arch+'-apple-macosx' in ir.read_text()
        args=['--target',arch+'-apple-darwin','--require-native']
        if opts.runtime_dir:
            args+=['--runtime-dir',opts.runtime_dir.resolve()]
        for backend in ('fast','llvm'):
            folder=root/backend
            folder.mkdir()
            out=folder/'app'
            obj=folder/'app.o' 
            mode=['--release'] if backend=='llvm' else ['--no-debug-info']
            run([bolt,'compile',program,'--backend',backend,*args,*mode,'--object-only','-o',obj])
            check_image(obj,cpu,1)
            run([bolt,'compile',program,'--backend',backend,*args,*mode,'-o',out])
            check_image(out,cpu,2)
            if backend=='fast':
                check_image(folder/'libBoltRuntime.dylib',cpu,6)
            else:
                assert 'libBoltRuntime' not in run(['/usr/bin/otool','-L',out])
        invalid=run([bolt,'compile',program,'--target',arch,'--runtime-dir',bolt.parent,'--rebuild','--backend','fast','-o',root/'wrong'],1)
        assert 'architecture' in invalid or 'slice' in invalid,invalid
    print('PASS macOS cross target selection, preprocessing, LLVM IR, Fast/LLVM objects, executables and runtime architecture checks')


if __name__=='__main__':
    main()
