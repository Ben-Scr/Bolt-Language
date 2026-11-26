#!/usr/bin/env python3
"""Stage the exact Darwin C++ runtime libraries selected by the Zig compiler driver."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--zig',required=True,type=Path)
    p.add_argument('--target',required=True,help='Darwin target, for example aarch64-macos.13.0')
    p.add_argument('--output',required=True,type=Path)
    args=p.parse_args()
    if not re.fullmatch(r'(x86_64|aarch64)-macos\.[0-9]+\.[0-9]+(?:\.[0-9]+)?',args.target):
        p.error('The target must select a 64-bit Mac and its deployment version')
    zig=args.zig.resolve()
    env=json.loads(subprocess.check_output([zig,'env'],text=True))
    cache=Path(env['global_cache_dir'])
    args.output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent,prefix='bolt-cxx-') as temp:
        folder=Path(temp)
        source=folder/'probe.cpp'
        source.write_text('#include <stdexcept>\nint main(int n,char**){try {if(n>1) throw std::runtime_error("bolt");}catch(...) {return 1;}return 0;}\n')
        build=subprocess.run([zig,'c++','-target',args.target,'-v','-O2',source,'-o',folder/'probe'],
                             stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,check=True)
        for name in ('libc++abi.a','libc++.a','libcompiler_rt.a'):
            # Zig's linker trace can print unquoted paths. The checked cache key
            # and archive name are independent of spaces in its base directory.
            match=re.search(r'[/\\]o[/\\]([0-9a-f]{32})[/\\]'+re.escape(name)+r'(?=\s|$)',build.stdout)
            if not match:
                raise ValueError(f'Zig did not report its {name} for {args.target}')
            archive=cache/'o'/match.group(1)/name
            with archive.open('rb') as f:
                if f.read(8)!=b'!<arch>\n':
                    raise ValueError(f'Invalid static library: {archive}')
            staged=folder/name
            shutil.copy2(archive,staged)
            staged.replace(args.output/name)


if __name__=='__main__':
    main()
