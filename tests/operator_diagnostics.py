#!/usr/bin/env python3
"""Shared operator diagnostics, recursive code generation and external tool failures."""
from __future__ import annotations
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
from lsp_protocol import Client


RECURSIVE = '''public struct Vec2
{
public:
    float X, Y;
    Vec2(float x, float y) { X = x; Y = y; }
    bool operator ==(const Vec2 a, const Vec2 b)
    {
        return a == b;
    }
}
public static class Program
{
public:
    int Main()
    {
        Vec2 a = Vec2(15f, 10f);
        Vec2 b = Vec2(15f, 2026f);
        if (a == b) return 1;
        return 0;
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', required=True, type=Path)
    parser.add_argument('--cxx', default='cl.exe' if os.name == 'nt' else 'c++')
    args = parser.parse_args()
    bolt = str(args.bolt.resolve())
    if os.name == 'nt':
        # The native recursion test must not open an interactive crash dialog.
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    count = 0

    def passed(label):
        nonlocal count
        count += 1
        print('PASS ' + label, flush=True)

    with tempfile.TemporaryDirectory(prefix='bolt-operator-') as directory:
        root = Path(directory)
        source = root / 'Program.bolt'
        source.write_text(RECURSIVE, encoding='utf-8')
        target = root / ('program.exe' if os.name == 'nt' else 'program')

        def invoke(*arguments, code=0, env=None):
            result = subprocess.run([bolt, *map(str, arguments)], cwd=root, env=env,
                                    capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180)
            if code is not None:
                assert result.returncode == code, (arguments, result.returncode, result.stdout, result.stderr)
            return result

        report = invoke('check', source, '--diagnostics', 'json')
        assert json.loads(report.stdout)['diagnostics'] == []
        client = Client(bolt)
        uri = source.as_uri()
        try:
            client.request('initialize', {'rootUri': root.as_uri(), 'capabilities': {}})
            client.send('initialized', {})
            client.send('textDocument/didOpen', {'textDocument': {
                'uri': uri, 'languageId': 'bolt', 'version': 1, 'text': RECURSIVE}})
            client.barrier()
            assert client.latest('textDocument/publishDiagnostics', uri)['diagnostics'] == []
            passed('recursive equality is valid in check and LSP, with implicit static operator syntax')

            invalid = RECURSIVE.replace('bool operator ==', 'int operator ==')
            source.write_text(invalid, encoding='utf-8')
            checked = json.loads(invoke('check', source, '--diagnostics', 'json', code=1).stdout)['diagnostics'][0]
            checked_text = invoke('check', source, code=1).stderr
            built = invoke('compile', source, '--no-timings', code=1).stderr
            assert checked_text == built and checked['code'] == 'BLT2210', (checked_text, built)
            client.send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': 2},
                                                 'contentChanges': [{'text': invalid}]})
            client.barrier()
            diagnostic = client.latest('textDocument/publishDiagnostics', uri)['diagnostics'][0]
            location = checked['location']
            assert diagnostic['code'] == checked['code'] and diagnostic['message'].startswith(checked['message'])
            assert diagnostic['range'] == {
                'start': {'line': location['line'] - 1, 'character': location['column'] - 1},
                'end': {'line': location['line'] - 1, 'character': location['column'] - 1 + location['length']}}, diagnostic
            assert location['length'] == 2 and location['file'].replace('\\', '/') == source.as_posix()
            passed('invalid comparison signature has identical check/build/LSP diagnostic and operator span')
            client.stop()
        finally:
            client.kill()

        source.write_text(RECURSIVE, encoding='utf-8')
        runtime = invoke('run', source, '--runtime', '--steps', '100', code=1)
        assert runtime.stderr.count('at Vec2.operator==') > 1 and '[runtime]' in runtime.stderr
        passed('the operator body resolves recursively, not to built-in field equality')
        caps = invoke('backends').stdout
        for backend, available in [('fast', 'Fast: available' in caps), ('llvm', 'LLVM: not available:' not in caps)]:
            if available:
                invoke('compile', source, '--backend', backend, '--object-only', '-o', root / (backend + '.obj'))
                passed(backend + ' accepts valid recursive operator code')
        compiled = invoke('compile', source, '--backend', 'cpp', '--cxx', args.cxx, '--no-pch', '-o', target)
        assert 'Built Debug' in compiled.stdout and 'Debug compilation:' in compiled.stderr
        crashed = invoke('run', source, '--no-compile', '-o', target, code=None)
        assert crashed.returncode != 0 and 'Application terminated' in crashed.stderr, (crashed.stdout, crashed.stderr)
        assert 'stack overflow' in crashed.stderr and 'Build/preparation failed' not in crashed.stderr
        passed('C++ builds recursive equality; running it reports an application crash separately from build status')

        source.write_text('public static class Program { public: int Main() { return -1; } }', encoding='utf-8')
        negative = invoke('run', source, '--backend', 'cpp', '--cxx', args.cxx, '--no-pch', '-o', target,
                          code=0xFFFFFFFF if os.name == 'nt' else 255)
        assert 'terminated' not in negative.stderr and 'exception' not in negative.stderr
        assert 'Built Debug' in negative.stdout and 'Debug compilation:' in negative.stderr
        passed('ordinary negative Main exit codes propagate without being misdiagnosed as an application crash')

        # A real child tool emits more than a pipe buffer on both streams and
        # uses a distinctive status, independent of a platform compiler's wording.
        helper_source = root / 'fake.cpp'
        helper = root / ('fake-cxx.exe' if os.name == 'nt' else 'fake-cxx')
        helper_source.write_text('''#include <cstdio>
#include <cstdlib>
int main() {
    for (int i=0; i<20000; ++i) { std::fputs("backend-stdout-line\\n", stdout); std::fputs("backend-stderr-line\\n", stderr); }
    std::fputs("backend-stdout-final\\n", stdout); std::fputs("backend-stderr-final\\n", stderr);
    const char* code=std::getenv("BOLT_TEST_TOOL_EXIT"); return code ? std::atoi(code) : 23;
}
''', encoding='utf-8')
        msvc = Path(args.cxx).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
        command = ([args.cxx, '/nologo', str(helper_source), '/Fe' + str(helper), '/Fo' + str(root / 'fake.obj')]
                   if msvc else [args.cxx, str(helper_source), '-o', str(helper)])
        helper_build = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
        assert helper_build.returncode == 0, (helper_build.stdout, helper_build.stderr)
        source.write_text('public static class Program { public: int Main() { return 0; } }', encoding='utf-8')
        old_binary = target.read_bytes()
        for mode in ('--debug', '--release'):
            direct = 'fast' if mode == '--debug' and 'Fast: available' in caps else 'llvm'
            for backend in ('cpp', direct):
                if backend == 'llvm' and 'LLVM: not available:' in caps:
                    continue
                selector = ['--cxx', helper, '--no-pch'] if backend == 'cpp' else ['--linker', helper]
                failure = invoke('compile', source, mode, '--backend', backend, *selector, '--rebuild', '-o', target, code=23)
                assert failure.stdout.count('backend-stdout-line') == 20000
                assert failure.stderr.count('backend-stderr-line') == 20000
                assert 'backend-stdout-final' in failure.stdout and 'backend-stderr-final' in failure.stderr
                assert 'failed with exit code 23' in failure.stderr and 'Build/preparation failed after:' in failure.stderr
                assert 'Built Debug' not in failure.stdout and 'Built Release' not in failure.stdout
                assert target.read_bytes() == old_binary
                passed(mode[2:] + ' ' + backend + ' preserves complete tool output, exit code, failed timing and old executable')

        # A stale staged executable cannot make a no-op tool look successful.
        staged = Path(str(target) + '.linking' + ('.exe' if os.name == 'nt' else ''))
        staged.write_bytes(old_binary)
        environment = dict(os.environ, BOLT_TEST_TOOL_EXIT='0')
        failure = invoke('compile', source, '--backend', 'cpp', '--cxx', helper, '--no-pch', '--rebuild', '-o', target,
                         code=1, env=environment)
        assert 'Built Debug' not in failure.stdout and 'Build/preparation failed after:' in failure.stderr
        assert target.read_bytes() == old_binary and not staged.exists()
        passed('zero exit status without a fresh executable cannot publish stale output or claim success')
    print(f'{count} operator/build diagnostic checks passed')


if __name__ == '__main__':
    main()
