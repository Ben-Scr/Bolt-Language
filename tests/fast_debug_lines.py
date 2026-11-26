#!/usr/bin/env python3
"""Check ELF Fast source positions at independent symbol addresses and inside both .bolt files."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(*command, expected=0):
    result = subprocess.run([str(s) for s in command], capture_output=True, text=True,
                            env=dict(os.environ, LC_ALL='C', BOLT_LLVM_LIBRARY='/missing/llvm-on-purpose'),
                            timeout=180, encoding='utf-8', errors='replace')
    if result.returncode != expected:
        raise AssertionError(f'{command}: exit {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result.stdout


def verify(binary, args, project):
    info = run(args.readelf, '-W', '--debug-dump=info', binary)
    table = run(args.readelf, '-W', '--debug-dump=decodedline', binary)
    symbols = run(args.nm, '-nS', binary)
    for filename, cls, method in [('Main.bolt', 'Program', 'Main'), ('Math.bolt', 'MathLib', 'Twice')]:
        textfile = project / 'src' / filename
        function = re.search(r'^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+[Tt]\s+b_'+cls+'_3a_3a'+method,
                             symbols, re.MULTILINE)
        assert function, 'missing function symbol ' + cls
        address = int(function[1], 16)
        end = address + int(function[2], 16)
        assert str(textfile)+':2' in run(args.addr2line, '-e', binary, hex(address)), (filename, hex(address))
        assert cls+'.'+method in info and filename in table, 'subprogram declaration is missing'
        found = set()
        for row in table.splitlines():
            entry = re.search(re.escape(filename)+r'\s+(\d+)\s+(0x[0-9a-fA-F]+|0)\b', row)
            if not entry:
                continue
            line, pc = int(entry[1]), int(entry[2], 16)
            if pc < address or pc >= end or line in found:
                continue
            assert str(textfile)+':'+str(line) in run(args.addr2line, '-e', binary, hex(pc))
            found.add(line)
        if not {2, 3, 4}.issubset(found):
            raise AssertionError(f'{filename}: missing statement lines in {found}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    parser.add_argument('--target', help='Optionally exercise x86_64-linux-gnu or aarch64-linux-gnu cross objects')
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--linker', type=Path)
    parser.add_argument('--runner', type=Path)
    parser.add_argument('--readelf', default='readelf')
    parser.add_argument('--addr2line', default='addr2line')
    parser.add_argument('--nm', default='nm')
    args = parser.parse_args()
    if bool(args.sdk) != bool(args.sysroot) or ((args.linker or args.runner) and not args.sdk):
        parser.error('Linked cross checks need a target SDK and sysroot')
    with tempfile.TemporaryDirectory(prefix='Bolt Zeilen ü ') as temp:
        work = Path(temp)
        project = work / 'Game'
        bolt = args.bolt.resolve()
        run(bolt, 'init', project)
        (project / 'src/Main.bolt').write_text('public static class Program {\n public: static int Main() {\n'
            '  int score = MathLib.Twice(21);\n  return score;\n }\n}\n', encoding='utf-8')
        (project / 'src/Math.bolt').write_text('public static class MathLib {\n public: static int Twice(int value) {\n'
            '  int scaled = value + value;\n  return scaled;\n }\n}\n', encoding='utf-8')
        flags = ['--target', args.target] if args.target else []
        command = [bolt, 'compile', project, '--debug', '--require-native', *flags]
        obj = work / 'debug.o'
        run(*command, '--object-only', '--debug-info', '-o', obj)
        verify(obj, args, project)
        raw = work / 'stripped.o'
        run(*command, '--backend', 'fast', '--object-only', '--no-debug-info', '-o', raw)
        assert '.debug_line' not in run(args.readelf, '-W', '-S', raw), 'no-debug-info emitted a line table'
        if not args.target or args.sdk:
            link = ['--runtime-dir', args.sdk.resolve(), '--sysroot', args.sysroot.resolve()] if args.sdk else []
            if args.linker:
                link.extend(['--linker', args.linker.resolve()])
            exe = work / 'Game with lines'
            output = run(*command, '--debug-info', *link, '-o', exe)
            assert '(Bolt Fast ' in output, output
            verify(exe, args, project)
            if not args.target or args.runner:
                run(*([args.runner.resolve()] if args.runner else []), exe, expected=42)
        print('PASS Fast DWARF4 declarations, statements, portable paths and executable on', args.target or 'host')


if __name__ == '__main__':
    main()
