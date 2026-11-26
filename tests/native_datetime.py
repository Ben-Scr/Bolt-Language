#!/usr/bin/env python3
"""Verify DateTime is source-authored Bolt which calls the host time ABI directly."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import time

PROGRAM = r'''import System
public static class Program {
public:
    void Main() {
        DateTime utc = DateTime.UtcNow;
        DateTime local = utc.ToLocalTime();
        DateTime today = DateTime.Today;
        Print(utc.ToUnixTimeSeconds());
        Print(utc.ToString());
        Print(local.ToString());
        Print(today.Hour);
        Print(today.Minute);
        Print(today.Second);
        Print(today.Millisecond);
        Print(utc.IsUtc);
        Print(local.IsUtc);
    }
}
'''

POSIX_KNOWN = r'''import System
public static class Program {
public:
    void Main() {
        Print(DateTime.FromUnixTimeSeconds(0l).ToLocalTime().ToString());
        Print(DateTime.FromUnixTimeSeconds(1768435200l).ToLocalTime().ToString());
        Print(DateTime.FromUnixTimeSeconds(1784073600l).ToLocalTime().ToString());
    }
}
'''


def calls(ir: str) -> set[str]:
    return set(re.findall(r'\bcall\s+[^\n@]*@([\w.$]+)\(', ir))


def run(command: list[str], cwd: Path, env: dict[str, str], timeout: int = 180) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                            text=True, encoding='utf-8', errors='replace', timeout=timeout)
    if result.returncode:
        raise AssertionError(f'{command}\nexit={result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}')
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bolt', required=True, type=Path)
    ap.add_argument('--cxx')
    ap.add_argument('--work', type=Path)
    args = ap.parse_args()
    bolt = args.bolt.resolve()
    host = platform.system()
    if host not in {'Linux', 'Darwin', 'Windows'}:
        print('SKIP: native DateTime test supports Linux, macOS and Windows')
        return 0
    platform_name = {'Linux': 'Linux', 'Darwin': 'MacOS', 'Windows': 'Windows'}[host]
    env = os.environ.copy()
    env.pop('BOLT_IDE_CONSOLE', None)
    env['PYTHONIOENCODING'] = 'utf-8'
    if host != 'Windows':
        env['TZ'] = 'UTC'

    temp = None
    if args.work:
        work = args.work.resolve(); work.mkdir(parents=True, exist_ok=True)
    else:
        temp = tempfile.TemporaryDirectory(prefix='bolt-native-datetime-')
        work = Path(temp.name)

    count = 0
    def passed(name: str) -> None:
        nonlocal count
        count += 1
        print('PASS ' + name, flush=True)

    try:
        source = work / 'DateTime.bolt'
        source.write_text(PROGRAM, encoding='utf-8')
        ir_path = work / 'DateTime.ll'
        run([str(bolt), 'emit-ir', str(source), '-o', str(ir_path), '--require-native', '--no-timings'], work, env)
        ir = ir_path.read_text(encoding='utf-8')
        direct_calls = calls(ir)
        if host in {'Linux', 'Darwin'}:
            needed = {'clock_gettime', 'localtime_r', 'mktime', 'malloc', 'free'}
        else:
            needed = {
                'GetSystemTimePreciseAsFileTime', 'FileTimeToSystemTime',
                'SystemTimeToFileTime', 'SystemTimeToTzSpecificLocalTime',
                'TzSpecificLocalTimeToSystemTime', 'GetProcessHeap', 'HeapAlloc', 'HeapFree'
            }
        missing = needed - direct_calls
        assert not missing, f'missing direct {platform_name} time calls: {sorted(missing)}'
        assert 'Clock.UnixTicks' not in ir and 'Bolt.Runtime.TimeZone' not in ir
        passed(f'{platform_name} DateTime lowers to direct platform ABI calls')

        caps = run([str(bolt), 'backends'], work, env).stdout
        backends: list[str] = []
        if 'Fast: available' in caps:
            backends.append('fast')
        if 'LLVM:' in caps and 'LLVM: not available:' not in caps:
            backends.append('llvm')
        assert backends, caps

        # The program emits nine lines: seconds, UTC text, local text, four midnight fields, two flags.
        def verify(result: subprocess.CompletedProcess[str], name: str) -> None:
            lines = [x.strip() for x in result.stdout.splitlines() if x.strip()]
            assert len(lines) == 9, (name, lines, result.stderr)
            now = int(lines[0])
            assert abs(now - int(time.time())) <= 15, (name, now, int(time.time()))
            assert lines[1].endswith('Z'), (name, lines[1])
            assert lines[3:7] == ['0', '0', '0', '0'], (name, lines[3:7])
            assert lines[7:] == ['true', 'false'], (name, lines[7:])
            passed(name)

        for backend in backends:
            exe = work / f'datetime-{backend}'
            flags = ['--backend', backend, '--require-native', '--no-timings', '--rebuild']
            if backend == 'fast':
                flags.append('--no-debug-info')
            run([str(bolt), 'compile', str(source), *flags, '-o', str(exe)], work, env)
            verify(run([str(exe)], work, env), f'{backend} AOT DateTime')
            jit = ['run', str(source), '--jit', '--backend', backend, '--require-native', '--no-timings']
            if backend == 'fast':
                jit.append('--no-debug-info')
            verify(run([str(bolt), *jit], work, env), f'{backend} JIT DateTime')

        if host in {'Linux', 'Darwin'}:
            known = work / 'KnownDateTime.bolt'
            known.write_text(POSIX_KNOWN, encoding='utf-8')
            exe = work / 'known-datetime'
            run([str(bolt), 'compile', str(known), '--require-native', '--backend', backends[-1],
                 '--no-timings', '-o', str(exe)], work, env)
            berlin = env.copy(); berlin['TZ'] = 'Europe/Berlin'
            lines = run([str(exe)], work, berlin).stdout.splitlines()
            assert lines == [
                '1970-01-01T01:00:00.0000000+01:00',
                '2026-01-15T01:00:00.0000000+01:00',
                '2026-07-15T02:00:00.0000000+02:00',
            ], lines
            passed('POSIX historical/local DST offsets')

        if args.cxx:
            cpp = work / 'DateTime.cpp'
            run([str(bolt), 'emit', str(source), '-o', str(cpp), '--no-timings'], work, env)
            generated = cpp.read_text(encoding='utf-8')
            assert 'bolt::platform::Clock::UnixTicks' not in generated
            assert 'bolt::platform::TimeZone' not in generated
            if host in {'Linux', 'Darwin'}:
                assert 'bolt::native::ffi_call' in generated and '::clock_gettime' in generated and '::localtime_r' in generated and '::mktime' in generated
            else:
                assert '::GetSystemTimePreciseAsFileTime' in generated
                assert '::SystemTimeToTzSpecificLocalTime' in generated
            passed('C++ compatibility emitter preserves source-authored direct DateTime ABI calls')

            exe = work / 'datetime-cpp'
            run([str(bolt), 'compile', str(source), '--backend', 'cpp', '--cxx', args.cxx,
                 '--no-timings', '--rebuild', '-o', str(exe)], work, env, 300)
            verify(run([str(exe)], work, env), 'C++ compatibility DateTime')

        # Interpreter is intentionally hosted in C++; this is a behavior regression check,
        # not evidence about the compiled DateTime path.
        verify(run([str(bolt), 'run', str(source), '--runtime', '--no-timings'], work, env),
               'interpreter DateTime compatibility')

        print(f'{count} native DateTime checks passed', flush=True)
        return 0
    finally:
        if temp:
            temp.cleanup()


if __name__ == '__main__':
    raise SystemExit(main())
