#!/usr/bin/env python3
"""Exercise the FreeBSD SDK entry points, real libc, dynamic loader, linker and Funke host."""
import argparse
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = '''import System
import System.Security.Cryptography
#if FreeBSD && X64
public static class Program {
public:
    int Main() {
        byte[] random = RandomNumberGenerator.GetBytes(513);
        Console.WriteLine("FreeBSD " + (string)random.Length);
        return 0;
    }
}
#else
WRONG_TARGET
#endif
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bolt', required=True, type=Path)
    args = p.parse_args()
    bolt = args.bolt.resolve()
    if platform.system() != 'FreeBSD' or platform.machine() != 'amd64':
        p.error('Run on FreeBSD/amd64')
    with tempfile.TemporaryDirectory(prefix='Bolt FreeBSD ') as temp:
        root = Path(temp)
        script = root / 'Main.bolt'
        source = ROOT / 'examples/FunkeBolt'
        script.write_text(PROGRAM)
        subprocess.run([bolt, 'check', script], check=True)
        for mode in (['--runtime'], ['--jit', '--backend', 'fast'], ['--jit', '--backend', 'llvm']):
            result = subprocess.run([bolt, 'run', script, *mode], text=True, capture_output=True)
            assert result.returncode == 0 and result.stdout.strip() == 'FreeBSD 513', result
        for name, options in [('fast', ['--debug', '--no-debug-info', '--backend', 'fast', '--require-native']),
                              ('llvm', ['--release', '--backend', 'llvm', '--require-native']),
                              ('cpp', ['--release', '--backend', 'cpp'])]:
            folder = root / name
            folder.mkdir()
            exe = folder / 'app'
            source_file = ROOT / 'tests/fixtures/freebsd-ffi.bolt' if name == 'cpp' else script
            subprocess.run([bolt, 'compile', source_file, *options, '-o', exe], check=True)
            expected = 'FreeBSD C ABI\n' if name == 'cpp' else 'FreeBSD 513\n'
            assert subprocess.check_output([exe], text=True) == expected
            libraries = subprocess.check_output(['ldd', exe], text=True)
            assert ('libBoltRuntime.so' in libraries) == (name == 'fast'), libraries
            assert 'not found' not in libraries, libraries
        shutil.copy2(source / 'Move.bolt', root / 'Move.bolt')
        host = root / 'FunkeHost'
        subprocess.run(['c++', '-std=c++20', '-I', ROOT / 'include', source / 'headless.cpp',
                        '-L', bolt.parent, '-lBoltCompiler', '-Wl,-rpath,' + str(bolt.parent),
                        '-o', host], check=True)
        output = subprocess.check_output([host, root / 'Move.bolt', root / 'Edited.bolt'], text=True)
        assert 'failed edit rollback' in output, output
    print('PASS FreeBSD interpreter, both JITs, three AOT backends, program dependencies and Funke hot reload')


if __name__ == '__main__':
    main()
