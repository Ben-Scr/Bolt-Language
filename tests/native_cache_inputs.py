#!/usr/bin/env python3
"""Verify source locations, transitive FFI header edits and saved-build compatibility."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def legacy_digest(path):
    value = 14695981039346656037
    for byte in path.read_bytes():
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--bolt', type=Path, required=True)
    cli.add_argument('--cxx', required=True)
    args = cli.parse_args()
    bolt = args.bolt.resolve()
    with tempfile.TemporaryDirectory(prefix='bolt cache inputs ') as directory:
        root = Path(directory)
        script = root / 'Main.bolt'
        target = root / 'program'
        environment = {**os.environ, 'BOLT_BUILD_ROOT': str(root / 'cache')}

        def invoke(*command):
            result = subprocess.run([str(x) for x in command], cwd=root, env=environment,
                                    text=True, capture_output=True, timeout=160)
            if result.returncode:
                raise AssertionError(f'{command}\n{result.stdout}{result.stderr}')
            return result.stdout

        def compile_native(backend, *options, destination=target):
            return invoke(bolt, 'compile', script, '--backend', backend, *options, '-o', destination)

        source = 'public static class Program {\npublic: static void Main() {\n    Print(42);\n}\n}\n'
        for backend in ('fast', 'llvm'):
            script.write_text(source)
            exe = root / backend
            flags = ('--debug', '--debug-info', '--require-native')
            compile_native(backend, *flags, destination=exe)
            assert 'Up to date' in compile_native(backend, *flags, destination=exe)
            script.write_text(source.replace('    Print', '\n' * 15 + '    Print'))
            assert 'Up to date' not in compile_native(backend, *flags, destination=exe)
            assert invoke(exe) == '42\n'
            if shutil.which('readelf'):
                lines = invoke('readelf', '--debug-dump=decodedline', exe)
                assert re.search(r'Main\.bolt\s+18\s+0x', lines), lines
            assert 'Up to date' in compile_native(backend, *flags, destination=exe)

        # Old installations wrote unprefixed FNV-1a digests. A compiler upgrade
        # must still permit running both linked artifacts from a previous build.
        for receipt in (root / 'cache').rglob('*.json'):
            record = json.loads(receipt.read_text())
            if record.get('product') != 'bolt-successful-aot-build-v1':
                continue
            record['hash'] = legacy_digest(Path(record['target']))
            for item in record['dependencies']:
                item['hash'] = legacy_digest(Path(item['path']))
            receipt.write_text(json.dumps(record))
        assert invoke(bolt, 'run', script, '--no-compile', '-o', root / 'llvm') == '42\n'

        script.write_text('[Header("outer.hpp"), Native("read_nested")] extern "C++" int ReadNested();\n'
                          'public static class Program {public: static void Main(){Print(ReadNested());}}')
        (root / 'outer.hpp').write_text('#include "inner.hpp"\ninline int read_nested(){return nested_value;}\n')
        header = root / 'inner.hpp'
        header.write_text('constexpr int nested_value = 31;\n')
        stamp = header.stat()
        flags = ('--no-pch', '--no-debug-info', '--cxx', args.cxx)
        compile_native('cpp', *flags)
        assert invoke(target) == '31\n'
        header.write_text('constexpr int nested_value = 32;\n')
        os.utime(header, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        assert 'Up to date' not in compile_native('cpp', *flags)
        assert invoke(target) == '32\n'
        assert 'Up to date' in compile_native('cpp', *flags)
    print('PASS native debug lines, legacy receipts and same-timestamp nested FFI header')


if __name__ == '__main__':
    main()
