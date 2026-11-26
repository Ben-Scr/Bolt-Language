#!/usr/bin/env python3
"""Check Bolt console methods, reusable Win32 FFI and byte-exact console I/O."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


OUTPUT_SOURCE = '''import System
public class Label : IStringable {
public:
    string ToString() { return "Label(42)"; }
}
static class Program {
public:
    void Main() {
        Console.Clear();
        Console.Write("");
        Console.Write("prefix:");
        Console.Write(42);
        Console.WriteLine();
        Console.WriteLine("Grüße Ελληνικά 日本語 😀");
        Console.WriteLine(Label());
        Console.WriteLine(true);
        Console.WriteLine("");
        Print("helper");
        Print();
        Console.Write("tail");
        Console.Clear();
    }
}
'''

INPUT_SOURCE = '''import System
static class Program {
public:
    void Main() {
        for (int i = 0; i < 4; i += 1) {
            string line = i == 1 ? Read() : Console.ReadLine();
            Console.Write("[");
            Console.Write(line);
            Console.WriteLine("]");
        }
    }
}
'''

IR_SOURCE = '''import System
static class Program {
public:
    void Main() {
        Console.WriteLine(Console.ReadLine());
        Console.Clear();
    }
}
'''

SCREEN_SOURCE = '''import System
static class Program {
public:
    void Main() {
        Console.WriteLine("Grüße Ω");
        Console.Write("next");
        Console.ReadLine();
        Console.Clear();
        Console.Write("cleared");
        Console.ReadLine();
        Console.Clear();
    }
}
'''

ERROR_SOURCE = '''import System
static class Program {
public:
    int Main(string[] args) {
        try {
            if (args.Length == 0) Console.ReadLine();
            else Console.Write("must fail");
        } catch (IOException& error) {
            return 42;
        }
        return 1;
    }
}
'''


EXTERNAL_SOURCE = '''extern "C" uint GetCurrentProcessId();
extern "C" void* GetCurrentProcess();
extern "C" int GetExitCodeProcess(void* process, uint& status);
extern "C" int CloseHandle(void* handle);
static class Program {
public:
    int Main() {
        uint status = 0u;
        bool active = GetExitCodeProcess(GetCurrentProcess(), status) != 0 && status == 259u;
        return active && GetCurrentProcessId() != 0 && CloseHandle(null) == 0 ? 0 : 1;
    }
}
'''

COMPATIBILITY_SOURCE = '''import System
static class Program {
public:
    void Main() {
        Console.Write("compat:");
        Console.WriteLine(42);
        Console.WriteLine(Read());
        Console.WriteLine(Console.ReadLine());
        Print("done");
    }
}
'''


def calls(ir: str) -> set[str]:
    return set(re.findall(r'\bcall\s+[^\n@]*@([\w.$]+)\(', ir))


def console_method_calls(ir: str) -> set[str]:
    """Only accept calls in ordinary methods whose debug source is Bolt code."""
    files = set(re.findall(
        r'^!(\d+) = !DIFile\(filename: "[^"\n]*WindowsConsole\.bolt",', ir, re.M))
    assert files, 'Console IR has no WindowsConsole.bolt source metadata'
    methods = set()
    for name, file in re.findall(
            r'^!\d+ = distinct !DISubprogram\(name: "[^"\n]*", '
            r'linkageName: "([\w.$]+)"[^\n]*?file: !(\d+),', ir, re.M):
        if file in files:
            methods.add(name)
    assert methods, 'WindowsConsole.bolt does not define compiled Bolt methods'
    called = set()
    for name, body in re.findall(
            r'^define [^\n]*@([\w.$]+)\([^\n]*\) [^\n]*\{\n(.*?)^\}', ir, re.M | re.S):
        if name in methods:
            called.update(calls(body))
    return called


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    parser.add_argument('--work', type=Path, help='Keep artifacts in this directory instead of a temporary directory.')
    parser.add_argument('--backend', choices=('fast', 'llvm'), action='append',
                        help='Only test this backend (repeat to select several).')
    parser.add_argument('--cxx', help='Also check the C++ compatibility backend with this compiler.')
    args = parser.parse_args()
    if os.name != 'nt':
        print('SKIP: direct Windows console tests require Windows')
        return 0
    bolt = args.bolt.resolve()
    checks = 0
    environment = os.environ.copy()
    environment['PYTHONIOENCODING'] = 'utf-8'
    environment.pop('BOLT_IDE_CONSOLE', None)

    def passed(name: str) -> None:
        nonlocal checks
        checks += 1
        print('PASS ' + name, flush=True)

    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
    with (nullcontext(str(args.work.resolve())) if args.work else
          tempfile.TemporaryDirectory(prefix='bolt-windows-console-')) as directory:
        work = Path(directory)

        def compiler(*options: object) -> subprocess.CompletedProcess[str]:
            command = [str(bolt), *map(str, options)]
            result = subprocess.run(command, cwd=work, env=environment,
                                    capture_output=True, text=True, encoding='utf-8',
                                    errors='replace', timeout=150)
            assert result.returncode == 0, (
                f'{command}\nexit {result.returncode}\n{result.stdout}\n{result.stderr}')
            return result

        capabilities = compiler('backends').stdout
        available = []
        if 'Fast: available' in capabilities:
            available.append('fast')
        if 'LLVM:' in capabilities and 'LLVM: not available:' not in capabilities:
            available.append('llvm')
        backends = args.backend or available
        assert backends, 'No direct backend available for Windows console tests'
        assert all(backend in available for backend in backends), capabilities

        ir_source = work / 'DirectCalls.bolt'
        ir_source.write_text(IR_SOURCE, encoding='utf-8')
        ir_path = work / 'DirectCalls.ll'
        compiler('emit-ir', ir_source, '-o', ir_path, '--require-native', '--no-timings')
        ir = ir_path.read_text(encoding='utf-8')
        called = calls(ir)
        required = {'GetStdHandle', 'GetConsoleMode', 'WriteFile', 'ReadFile',
                    'WriteConsoleW', 'ReadConsoleW', 'GetConsoleScreenBufferInfo',
                    'FillConsoleOutputCharacterW', 'FillConsoleOutputAttribute',
                    'SetConsoleCursorPosition'}
        assert required <= called, f'Missing direct Win32 calls: {sorted(required - called)}'
        # br_library is still valid for the existing exception object runtime;
        # terminal reads and writes must go through the Win32 declarations.
        assert not ({'br_write', 'br_read'} & called), 'Console still calls the C++ I/O bridge'
        assert 'b_windows_console_' not in ir, 'Console still uses compiler-generated console helpers'
        from_bolt = console_method_calls(ir)
        assert required <= from_bolt, (
            f'Win32 calls outside ordinary WindowsConsole.bolt methods: {sorted(required - from_bolt)}')
        passed('WindowsConsole.bolt methods call Win32 directly without generated console helpers')

        external_source = work / 'ExternalCalls.bolt'
        external_source.write_text(EXTERNAL_SOURCE, encoding='utf-8')
        external_ir_path = work / 'ExternalCalls.ll'
        compiler('emit-ir', external_source, '-o', external_ir_path, '--require-native', '--no-timings')
        external_ir = external_ir_path.read_text(encoding='utf-8')
        assert {'GetCurrentProcessId', 'GetCurrentProcess', 'GetExitCodeProcess', 'CloseHandle'} <= calls(external_ir), (
            'Ordinary extern C functions did not lower to their native symbols')
        assert 'GetStdHandle' not in external_ir, 'Non-console extern C unexpectedly requires Console'
        passed('Ordinary extern C declarations lower directly without Console')

        output_source = work / 'Output.bolt'
        input_source = work / 'Input.bolt'
        screen_source = work / 'Screen.bolt'
        error_source = work / 'Errors.bolt'
        output_source.write_text(OUTPUT_SOURCE, encoding='utf-8')
        input_source.write_text(INPUT_SOURCE, encoding='utf-8')
        screen_source.write_text(SCREEN_SOURCE, encoding='utf-8')
        error_source.write_text(ERROR_SOURCE, encoding='utf-8')
        output_expected = ('prefix:42\nGrüße Ελληνικά 日本語 😀\nLabel(42)\n'
                           'true\n\nhelper\n\ntail').encode('utf-8')
        long_line = ('x' * 4097 + 'ö😀' * 2048 + 'tail').encode('utf-8')
        cases = [
            ('LF lines, empty line and Read helper', b'first\n\nthird\nfourth\n',
             [b'first', b'', b'third', b'fourth']),
            ('CRLF and UTF-8', 'Grüße 😀\r\n日本語\r\n\r\nlast\r\n'.encode('utf-8'),
             ['Grüße 😀'.encode('utf-8'), '日本語'.encode('utf-8'), b'', b'last']),
            ('EOF is repeatably empty', b'', [b'', b'', b'', b'']),
            ('unterminated final line', b'first\nlast', [b'first', b'last', b'', b'']),
            ('long UTF-8 line across input buffers', long_line + b'\r\nnext\n',
             [long_line, b'next', b'', b'']),
            ('embedded NUL remains data', b'a\x00b\nc\n', [b'a\x00b', b'c', b'', b'']),
        ]

        def execute(command: list[str], incoming: bytes, expected: bytes, name: str) -> None:
            result = subprocess.run(command, input=incoming, cwd=work,
                                    env=environment, capture_output=True, timeout=30)
            assert result.returncode == 0, (
                f'{name}: exit {result.returncode}\n{result.stderr!r}')
            assert result.stdout == expected, (
                f'{name}: output mismatch\nexpected={expected[:500]!r} '
                f'({len(expected)} bytes)\nactual={result.stdout[:500]!r} '
                f'({len(result.stdout)} bytes)\nstderr={result.stderr!r}')
            assert not result.stderr, f'{name}: unexpected stderr {result.stderr!r}'
            passed(name)

        for backend in backends:
            flags = ['--backend', backend, '--require-native', '--no-timings']
            flags += ['--release'] if backend == 'llvm' else ['--no-debug-info']
            output_exe = work / f'output-{backend}.exe'
            input_exe = work / f'input-{backend}.exe'
            screen_exe = work / f'screen-{backend}.exe'
            error_exe = work / f'errors-{backend}.exe'
            external_exe = work / f'external-{backend}.exe'
            compiler('compile', output_source, *flags, '-o', output_exe)
            compiler('compile', input_source, *flags, '-o', input_exe)
            compiler('compile', screen_source, *flags, '-o', screen_exe)
            compiler('compile', error_source, *flags, '-o', error_exe)
            compiler('compile', external_source, *flags, '-o', external_exe)
            native_outputs = (output_exe, input_exe, screen_exe, error_exe, external_exe)
            assert not any(Path(str(output) + '.bolt.cpp').exists() for output in native_outputs), (
                'Native Console emitted C++ source')
            execute([str(external_exe)], b'', b'',
                    f'{backend} AOT: reusable scalar/pointer/reference extern C Win32 calls')
            execute([str(bolt), 'run', str(external_source), '--jit', *flags], b'', b'',
                    f'{backend} JIT: reusable scalar/pointer/reference extern C Win32 calls')
            execute([str(output_exe)], b'', output_expected,
                    f'{backend} AOT: Write/WriteLine, IStringable, Unicode, redirected Clear')
            for name, incoming, lines in cases:
                expected = b''.join(b'[' + line + b']\n' for line in lines)
                execute([str(input_exe)], incoming, expected, f'{backend} AOT: {name}')
            execute([str(bolt), 'run', str(output_source), '--jit', *flags], b'',
                    output_expected, f'{backend} JIT: direct Console output')
            _, incoming, lines = cases[1]
            expected = b''.join(b'[' + line + b']\n' for line in lines)
            execute([str(bolt), 'run', str(input_source), '--jit', *flags], incoming,
                    expected, f'{backend} JIT: direct Console input')
            denied = work / f'denied-{backend}.txt'
            with denied.open('wb') as write_only:
                result = subprocess.run([str(error_exe)], stdin=write_only,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        cwd=work, env=environment, timeout=30)
            assert result.returncode == 42 and not result.stdout and not result.stderr, (
                f'ReadLine failed to throw IOException: {result}')
            passed(f'{backend} AOT: ReadLine access failure throws IOException')
            with denied.open('rb') as read_only:
                result = subprocess.run([str(error_exe), 'write'], stdin=subprocess.DEVNULL,
                                        stdout=read_only, stderr=subprocess.PIPE,
                                        cwd=work, env=environment, timeout=30)
            assert result.returncode == 42 and not result.stderr, (
                f'Write failed to throw IOException: {result}')
            passed(f'{backend} AOT: Write access failure throws IOException')
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
            worker = Path(__file__).with_name('windows_native_console_host.py')
            result = subprocess.run([sys.executable, str(worker), str(screen_exe), str(input_exe)],
                                    env=environment, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=60,
                                    creationflags=subprocess.CREATE_NEW_CONSOLE,
                                    startupinfo=startup)
            assert result.returncode == 0, result.stdout + result.stderr
            for line in result.stdout.splitlines():
                assert line.startswith('PASS '), line
                passed(f'{backend} AOT: {line[5:]}')

        compatibility_source = work / 'Compatibility.bolt'
        compatibility_source.write_text(COMPATIBILITY_SOURCE, encoding='utf-8')
        compatibility_modes = [('interpreter', [str(bolt), 'run', str(compatibility_source),
                                                '--runtime', '--no-timings'])]
        if args.cxx:
            compatibility_exe = work / 'compatibility-cpp.exe'
            compiler('compile', compatibility_source, '--backend', 'cpp', '--cxx', args.cxx,
                     '--no-timings', '-o', compatibility_exe)
            compatibility_modes.append(('C++', [str(compatibility_exe)]))
        for name, command in compatibility_modes:
            result = subprocess.run(
                command, input=b'first\nsecond\n', cwd=work, env=environment,
                capture_output=True, timeout=150)
            assert result.returncode == 0 and not result.stderr, (
                f'{name}: exit {result.returncode}, stderr={result.stderr!r}')
            assert result.stdout.replace(b'\r\n', b'\n') == b'compat:42\nfirst\nsecond\ndone\n', (
                f'{name}: compatibility Console output mismatch: {result.stdout!r}')
            passed(f'{name}: Console output/input and Print/Read compatibility')

        # The interpreter's Console fallback must not accidentally admit user
        # code that depends on unsupported exceptions or foreign functions.
        unsupported = [
            ('exceptions', '', 'try { Console.WriteLine("x"); } catch (Exception& error) {}'),
            ('native object', '', 'Exception error = Exception("x"); Console.WriteLine("x");'),
            ('foreign call', 'extern "C" uint GetCurrentProcessId();',
             'Console.WriteLine(GetCurrentProcessId());'),
            ('stdlib foreign call', '',
             'Bolt.Runtime.Windows.GetStdHandle(4294967285u); Console.WriteLine("x");'),
        ]
        for name, declarations, body in unsupported:
            source = work / ('Unsupported-' + name.replace(' ', '-') + '.bolt')
            source.write_text('import System\n' + declarations +
                              '\nstatic class Program { public: void Main() { ' + body + ' } }\n',
                              encoding='utf-8')
            result = subprocess.run([str(bolt), 'run', str(source), '--runtime', '--no-timings'],
                                    cwd=work, env=environment, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=30)
            assert result.returncode != 0 and '--runtime does not support' in result.stderr.lower(), (
                f'Interpreter accepted unsupported {name} or failed for another reason: {result}')
            passed(f'interpreter: Console does not admit unsupported {name}')

    print(f'{checks} Windows native console checks passed', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
