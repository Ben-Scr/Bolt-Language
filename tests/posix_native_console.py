#!/usr/bin/env python3
"""Verify Linux/macOS Console is Bolt code which calls the POSIX ABI directly."""
from __future__ import annotations

import argparse
from pathlib import Path
import os
import platform
import re
import subprocess
import tempfile

SOURCE = r'''import System
public class Label : IStringable {
public:
    string ToString() { return "Label(42)"; }
}
static class Program {
public:
    void Main() {
        Console.Clear();
        Console.Write("prefix:");
        Console.Write(42);
        Console.WriteLine();
        Console.WriteLine("Grüße Ελληνικά 日本語 😀");
        Console.WriteLine(Label());
        string first = Console.ReadLine();
        string second = Console.ReadLine();
        Console.Write("["); Console.Write(first); Console.WriteLine("]");
        Console.Write("["); Console.Write(second); Console.WriteLine("]");
        Console.Clear();
    }
}
'''

IR_SOURCE = r'''import System
static class Program {
public:
    void Main() {
        Console.WriteLine(Console.ReadLine());
        Console.Clear();
    }
}
'''

EXTERN_SOURCE = r'''[Header("unistd.h"), Native("getpid")]
extern "C" int GetPid();
static class Program { public: int Main() { return GetPid() > 0 ? 0 : 1; } }
'''


def calls(ir: str) -> set[str]:
    return set(re.findall(r'\bcall\s+[^\n@]*@([\w.$]+)\(', ir))


def source_method_calls(ir: str, source_name: str) -> set[str]:
    files = set(re.findall(
        rf'^!(\d+) = !DIFile\(filename: "[^"\n]*{re.escape(source_name)}",', ir, re.M))
    assert files, f'IR has no {source_name} source metadata'
    methods = set()
    for name, file_id in re.findall(
            r'^!\d+ = distinct !DISubprogram\(name: "[^"\n]*", '
            r'linkageName: "([\w.$]+)"[^\n]*?file: !(\d+),', ir, re.M):
        if file_id in files:
            methods.add(name)
    assert methods, f'{source_name} defines no compiled Bolt methods'
    result: set[str] = set()
    for name, body in re.findall(
            r'^define [^\n]*@([\w.$]+)\([^\n]*\) [^\n]*\{\n(.*?)^\}', ir, re.M | re.S):
        if name in methods:
            result.update(calls(body))
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bolt', type=Path, required=True)
    ap.add_argument('--cxx')
    ap.add_argument('--work', type=Path)
    args = ap.parse_args()

    system = platform.system()
    if system not in {'Linux', 'Darwin'}:
        print('SKIP: POSIX console test requires Linux or macOS')
        return 0
    platform_name = 'Linux' if system == 'Linux' else 'MacOS'
    console_source = platform_name + 'Console.bolt'
    bolt = args.bolt.resolve()
    env = os.environ.copy()
    env.pop('BOLT_IDE_CONSOLE', None)
    env['PYTHONIOENCODING'] = 'utf-8'
    checks = 0

    def passed(name: str) -> None:
        nonlocal checks
        checks += 1
        print('PASS ' + name, flush=True)

    context = None
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        directory = str(args.work.resolve())
    else:
        context = tempfile.TemporaryDirectory(prefix='bolt-posix-console-')
        directory = context.name
    try:
        work = Path(directory)

        def compiler(*options: object) -> subprocess.CompletedProcess[str]:
            command = [str(bolt), *map(str, options)]
            result = subprocess.run(command, cwd=work, env=env, capture_output=True,
                                    text=True, encoding='utf-8', errors='replace', timeout=180)
            assert result.returncode == 0, (
                f'{command}\nexit {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')
            return result

        caps = compiler('backends').stdout
        available: list[str] = []
        if 'Fast: available' in caps:
            available.append('fast')
        if 'LLVM:' in caps and 'LLVM: not available:' not in caps:
            available.append('llvm')
        assert available, caps

        ir_file = work / 'DirectConsole.bolt'
        ir_file.write_text(IR_SOURCE, encoding='utf-8')
        ir_path = work / 'DirectConsole.ll'
        compiler('emit-ir', ir_file, '-o', ir_path, '--require-native', '--no-timings')
        ir = ir_path.read_text(encoding='utf-8')
        all_calls = calls(ir)
        required = {'write', 'read', 'isatty', 'malloc', 'realloc', 'free'}
        missing = required - all_calls
        assert not missing, f'Missing direct POSIX calls: {sorted(missing)}'
        assert not ({'br_write', 'br_read'} & all_calls), 'Console still uses the C++ I/O bridge'
        assert 'bolt::platform::Terminal' not in ir and 'Bolt.Runtime.Terminal' not in ir
        direct = source_method_calls(ir, console_source)
        assert {'write', 'read', 'isatty'} <= direct, (
            f'POSIX terminal calls are not emitted from {console_source}: '
            f'{sorted({"write", "read", "isatty"} - direct)}')
        passed(f'{console_source} lowers to direct POSIX I/O with no C++ terminal bridge')

        external = work / 'External.bolt'
        external.write_text(EXTERN_SOURCE, encoding='utf-8')
        external_ir = work / 'External.ll'
        compiler('emit-ir', external, '-o', external_ir, '--require-native', '--no-timings')
        assert 'getpid' in calls(external_ir.read_text(encoding='utf-8'))
        passed('ordinary POSIX extern C declarations lower directly')

        source = work / 'Console.bolt'
        source.write_text(SOURCE, encoding='utf-8')
        incoming = ('first\r\n' + ('x' * 1024) + 'ö😀tail\n').encode('utf-8')
        expected = ('prefix:42\nGrüße Ελληνικά 日本語 😀\nLabel(42)\n'
                    '[first]\n[' + ('x' * 1024) + 'ö😀tail]\n').encode('utf-8')

        def execute(command: list[str], name: str, data: bytes = incoming,
                    wanted: bytes = expected) -> None:
            result = subprocess.run(command, cwd=work, env=env, input=data,
                                    capture_output=True, timeout=120)
            assert result.returncode == 0, f'{name}: exit={result.returncode}, stderr={result.stderr!r}'
            assert result.stdout == wanted, (
                f'{name}: output mismatch\nexpected={wanted!r}\nactual={result.stdout!r}\n'
                f'stderr={result.stderr!r}')
            assert result.stderr == b'', f'{name}: unexpected stderr: {result.stderr!r}'
            passed(name)

        for backend in available:
            exe = work / f'console-{backend}'
            flags = ['--backend', backend, '--require-native', '--no-timings']
            if backend == 'llvm':
                flags.append('--release')
            else:
                flags.append('--no-debug-info')
            compiler('compile', source, *flags, '-o', exe)
            assert not Path(str(exe) + '.bolt.cpp').exists(), 'native backend unexpectedly emitted C++'
            execute([str(exe)], f'{backend} AOT Console output/input')
            jit_flags = ['--jit', '--backend', backend, '--require-native', '--no-timings']
            execute([str(bolt), 'run', str(source), *jit_flags], f'{backend} JIT Console output/input')

        if args.cxx:
            cpp = work / 'Console.cpp'
            compiler('emit', source, '-o', cpp)
            generated = cpp.read_text(encoding='utf-8')
            assert 'bolt::platform::Terminal' not in generated
            assert 'bolt::native::ffi_call' in generated and '::write' in generated and '::read' in generated and '::isatty' in generated
            passed('C++ compatibility emission contains source-authored POSIX calls, not Terminal')
            exe = work / 'console-cpp'
            compiler('compile', source, '--backend', 'cpp', '--cxx', args.cxx,
                     '--no-timings', '-o', exe)
            execute([str(exe)], 'C++ compatibility Console output/input')

        # Interpreter intentionally remains an interpreter hosted in C++; it is
        # not evidence for or against compiled Console ABI decoupling. Keep its
        # user-visible Console behavior working as a regression check.
        interpreter_in = ('first\n' + ('x' * 1024) + 'ö😀tail\n').encode('utf-8')
        execute([str(bolt), 'run', str(source), '--runtime', '--no-timings'],
                'interpreter Console compatibility', interpreter_in, expected)

        print(f'{checks} POSIX native console checks passed', flush=True)
        return 0
    finally:
        if context:
            context.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
