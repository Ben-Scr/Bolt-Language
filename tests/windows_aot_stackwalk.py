#!/usr/bin/env python3
"""Link a fresh Fast COFF object to a compiled Windows consumer and walk its frames."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emitter', type=Path, required=True)
    parser.add_argument('--cxx', type=Path, required=True)
    args = parser.parse_args()
    emitter = args.emitter.resolve()
    # Match the compiler under test even if Windows ARM runs an x64 Python.
    with emitter.open('rb') as stream:
        stream.seek(60)
        stream.seek(int.from_bytes(stream.read(4), 'little'))
        signature, machine = struct.unpack('<4sH', stream.read(6))
    if signature != b'PE\0\0' or machine not in (0x8664, 0xaa64):
        raise ValueError('Expected a Windows x64 or ARM64 object emitter')
    arch = 'arm64' if machine == 0xaa64 else 'x64'
    source = Path(__file__).with_name('WindowsAotStackwalk.cpp').resolve()
    with tempfile.TemporaryDirectory(prefix='Bolt Windows stackwalk ') as directory:
        work = Path(directory)
        obj, exe = work / 'generated.obj', work / 'stackwalk.exe'
        commands = [[emitter, obj, arch]]
        if args.cxx.stem.lower() in ('cl', 'clang-cl'):
            commands.append([args.cxx, '/nologo', '/std:c++20', '/O2', '/EHsc', source, obj, '/Fe:'+str(exe)])
        else:
            commands.append([args.cxx, '-std=c++20', '-O2', '-static', source, obj, '-o', exe])
        commands.append([exe])
        for command in commands:
            subprocess.run(command, check=True, cwd=work, timeout=100)


if __name__ == '__main__':
    main()
