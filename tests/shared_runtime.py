#!/usr/bin/env python3
"""Direct runtime/ABI and CLI timing checks. Does not invoke a user C++ frontend.
A compatible native linker/SDK is still required for AOT builds.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bolt', type=Path, required=True)
    ap.add_argument('--timeout', type=int, default=120, help='Maximum seconds per compiler or program invocation')
    a = ap.parse_args()
    if a.timeout <= 0: ap.error('--timeout must be positive')
    bolt = str(a.bolt.resolve()); checks = 0
    def ok(name):
        nonlocal checks
        checks += 1; print('PASS ' + name, flush=True)
    with tempfile.TemporaryDirectory(prefix='bolt-shared-runtime-') as directory:
        work = Path(directory)
        def run(command, code=0, env=None):
            result = subprocess.run(list(map(str, command)), cwd=work, env=env,
                text=True, encoding='utf-8', capture_output=True, timeout=a.timeout)
            if result.returncode != code:
                raise AssertionError(f'{command}\nexpected {code}, got {result.returncode}\n{result.stdout}\n{result.stderr}')
            return result
        source = work / 'Program.bolt'
        shutil.copy2(ROOT / 'tests/shared_runtime/Features.bolt', source)
        availability = run([bolt, 'backends']).stdout
        backends = []
        if 'Fast: available' in availability: backends.append('fast')
        if 'LLVM: not available:' not in availability: backends.append('llvm')
        if not backends: raise AssertionError('No direct backend available for these tests')
        for backend in backends:
            for jit in (True, False):
                command = [bolt, 'run', source, '--backend', backend, '--require-native']
                command += ['--jit'] if jit else ['--native']
                if backend == 'llvm': command += ['--release']
                report = run([*command, '--', 'two words', 'Ä😀', ''])
                assert '\n'.join(line for line in report.stdout.splitlines() if not line.startswith('Built ')) == 'Shared runtime assertions: 85', report.stdout
                assert 'C++ native backend selected' not in report.stderr, report.stderr
                assert re.search(r'\[bolt\] (JIT compilation|Debug compilation|Release compilation): [0-9.]+ ms', report.stderr), report.stderr
                assert not (work / 'data').exists(), 'program cleanup failed'
                assert not list(work.rglob('*.bolt.cpp')), 'direct build emitted C++ source'
                ok(backend + (' JIT' if jit else ' AOT') + ': 85 library/value/range/argv assertions, no generated C++')
        # Normal file operations automatically select a direct backend, not cpp.
        simple = work / 'Simple.bolt'
        simple.write_text('import System.IO\nstatic class Program{public:void Main(){File.WriteAllText("simple.txt","ok");Print(File.ReadAllText("simple.txt"));}}', encoding='utf-8')
        out = run([bolt, 'run', simple, '--require-native'])
        assert out.stdout.endswith('ok\n') and 'C++' not in out.stderr, out
        ok('automatic direct backend for File without --backend')
        out = run([bolt, 'run', simple, '--require-native'])
        assert 'cache hit' in out.stderr and 'no code generation/linking' in out.stderr, out.stderr
        ok('cache hit reports preparation time without claiming code generation')
        out = run([bolt, 'run', simple, '--require-native', '--no-timings'])
        assert ' ms' not in out.stderr, out.stderr
        ok('--no-timings suppresses time output')
        out = run([bolt, 'compile', simple, '--require-native', '--timings'])
        assert 'compilation:' in out.stderr, out.stderr
        ok('compile --timings explicit flag (also enabled by default)')
        # Stream merged output to check the timing line precedes user output.
        merged = subprocess.run([bolt, 'run', str(simple), '--require-native'], cwd=work,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', timeout=a.timeout)
        assert merged.returncode == 0 and merged.stdout.index('compilation:') < merged.stdout.index('\nok\n'), merged.stdout
        ok('compilation timing printed before application output')
        interp = work / 'Interpreter.bolt'
        interp.write_text('static class Program{public:void Main(){Print(7);}}')
        out = run([bolt, 'run', interp, '--runtime'])
        assert 'interpreter preparation' in out.stderr and out.stdout == '7\n', out
        ok('interpreted run reports analysis, not fictional native compilation')
        fail = work / 'Fail.bolt'
        fail.write_text('static class Program{public:void Main(){int n}}')
        out = run([bolt, 'run', fail], code=1)
        assert 'Build/preparation failed after:' in out.stderr, out.stderr
        ok('failed analysis reports elapsed preparation')
        # A fake frontend ensures the direct path cannot silently use one.
        if os.name != 'nt':
            traps = work / 'frontends'; traps.mkdir(); marker = work / 'frontend-called'
            for compiler in ('c++','g++','clang++','clang','gcc','cc','cl'):
                path=traps/compiler; path.write_text(f'#!/bin/sh\necho called >> "{marker}"\nexit 97\n'); path.chmod(0o755)
            env = dict(os.environ); env['PATH'] = str(traps)+os.pathsep+env.get('PATH','')
            for backend in backends:
                out = run([bolt, 'compile', simple, '--require-native', '--backend', backend, '-o', work/('no-frontend-'+backend)], env=env)
                assert not marker.exists(), 'direct compilation invoked a C/C++ frontend'
                ok(backend+' AOT builds with unusable C/C++ frontend commands in PATH')
        failures = {
            'missing-file': ('import System.IO\nstatic class Program{public:void Main(){Print(File.ReadAllText("absent.txt"));}}', 'absent'),
            'shift-negative': ('static class Program{public:void Main(){int shift=-1;Print(1<<shift);}}', 'shift'),
            'char-surrogate': ('static class Program{public:void Main(){int n=55296;Print((char)n);}}', 'char'),
            'mutated-range': ('import System.Collections.Generic\nstatic class Program{public:void Main(){List<int> values;values.Add(1);for(x:values){values.Add(2);}}}', 'mutat'),
        }
        for name,(text,expected) in failures.items():
            path=work/(name+'.bolt');path.write_text(text,encoding='utf-8')
            out=run([bolt,'run',path,'--jit','--backend',backends[0],'--require-native'], code=1)
            assert expected.lower() in out.stderr.lower(), out.stderr
            ok('direct diagnostic: '+name)
        example = work / 'AsyncJson.bolt'
        shutil.copy2(ROOT / 'examples/Stdlib11/AsyncJson/Program.bolt', example)
        entry = work / 'AsyncEntry.bolt'
        entry.write_text('import System.Threading.Tasks\nstatic class Program{public:'
                         'async Task<int> Main(string[] args){await Task.Delay(1);return args.Length+5;}}')
        for backend in backends:
            for mode in (['--jit'], ['--release'] if backend == 'llvm' else []):
                options = ['--backend', backend, '--require-native', '--no-timings', *mode]
                target = work / 'measurement.json'
                report = run([bolt, 'run', example, *options, '--', target])
                assert report.stdout.endswith('Bolt: 42\nBJS1 bytes: 61\n'), report.stdout
                assert json.loads(target.read_text(encoding='utf-8')) == {'Name': 'Bolt', 'Result': 42}
                target.unlink()
                run([bolt, 'run', entry, *options, '--', 'one', 'two'], code=7)
                ok(f'{backend} {mode}: async entry status, awaited I/O and reflected JSON/BJS1')
    print(f'{checks} shared-runtime/CLI groups passed',flush=True)
    return 0
if __name__=='__main__': raise SystemExit(main())
