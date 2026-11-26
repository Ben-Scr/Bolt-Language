#!/usr/bin/env python3
"""Reproducible differential test of Bolt Queue<int> against collections.deque.

Builds standalone Bolt sources under a temporary directory. Does not edit live builds.
"""
import argparse
from collections import deque
from pathlib import Path
import os
import random
import subprocess
import tempfile
import json
import sys


def generate(seed: int, count: int):
    rng = random.Random(seed)
    reference = deque()
    operations = []
    answers = []
    # An initial fill exercises the first transition between the two buffer stacks.
    for index in range(17):
        operations.append(f'q.Enqueue({index});')
        reference.append(index)
    for _ in range(count):
        operation = rng.choices(['enqueue', 'dequeue', 'peek', 'clear', 'reserve', 'trydequeue', 'array', 'iterate'], [34, 25, 14, 3, 5, 10, 5, 4])[0]
        if not reference and operation in ('dequeue', 'peek'):
            operation = 'enqueue'
        if operation == 'enqueue':
            value = rng.randrange(-100000, 100000)
            operations.append(f'q.Enqueue({value});')
            reference.append(value)
        elif operation == 'dequeue':
            operations.append('Print(q.Dequeue());')
            answers.append(str(reference.popleft()))
        elif operation == 'peek':
            operations.append('Print(q.Peek());')
            answers.append(str(reference[0]))
        elif operation == 'clear':
            operations.append('q.Clear();')
            reference.clear()
        elif operation == 'reserve':
            operations.append(f'q.Reserve({rng.randrange(0, 100)});')
        elif operation == 'trydequeue':
            operations.append('int extracted = -987654; Print(q.TryDequeue(extracted)); Print(extracted);')
            if reference:
                answers += ['true', str(reference.popleft())]
            else:
                answers += ['false', '-987654']
        elif operation == 'array':
            operations.append('int[] values = q.ToArray(); Print(values.Length); for(int j=0;j<values.Length;j++) Print(values[j]);')
            answers.append(str(len(reference)))
            answers.extend(map(str, reference))
        elif operation == 'iterate':
            operations.append('QueueEnumerator<int> it = q.GetEnumerator(); while(it.MoveNext()) Print(it.Current); Print(999999);')
            answers.extend(map(str, reference))
            answers.append('999999')
    operations.append('Print(q.Count);')
    answers.append(str(len(reference)))
    source = 'import System.Collections.Generic\nstatic class Program { public: void Main() {\nQueue<int> q;\n' + '\n'.join('{' + x + '}' for x in operations) + '\n} }\n'
    return source, '\n'.join(answers) + '\n'


def save_failure(directory, seed, profile, source, expected, result, stage):
    if directory:
        directory.mkdir(parents=True, exist_ok=True)
    target = Path(tempfile.mkdtemp(prefix=f'queue-{seed}-{profile}-', dir=directory))
    (target / 'Program.bolt').write_text(source, encoding='utf-8')
    (target / 'expected.txt').write_text(expected, encoding='utf-8')
    (target / f'{stage}.stdout').write_text(result.stdout or '', encoding='utf-8')
    (target / f'{stage}.stderr').write_text(result.stderr or '', encoding='utf-8')
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    parser.add_argument('--count', type=int, default=250)
    parser.add_argument('--seeds', nargs='+', type=int, default=[3, 17, 42])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--failure-dir', type=Path, help='Parent for persistent failure reproductions')
    args = parser.parse_args()
    if args.count <= 0:
        parser.error('--count must be positive')
    compiler = args.bolt.resolve(strict=True)
    results = []
    with tempfile.TemporaryDirectory(prefix='bolt-queue-differential-') as tmp:
        directory = Path(tmp)
        for seed in args.seeds:
            source, expected = generate(seed, args.count)
            fixture = directory / f'QueueFuzz{seed}.bolt'
            fixture.write_text(source, encoding='utf-8')
            for profile in ('debug', 'release'):
                executable = directory / f'{fixture.stem}-{profile}'
                if os.name == 'nt':
                    executable = executable.with_suffix('.exe')
                build = [str(compiler), 'compile', fixture.name, '--' + profile,
                         '--require-native', '-o', str(executable)]
                stages = [('compile', build, 120), ('run', [str(executable)], 30)]
                case = {'seed': seed, 'release': profile == 'release', 'operations': args.count,
                        'expected_lines': len(expected.splitlines())}
                for stage, command, timeout in stages:
                    try:
                        result = subprocess.run(command, cwd=directory, capture_output=True,
                                                text=True, encoding='utf-8', errors='replace', timeout=timeout)
                    except subprocess.TimeoutExpired as error:
                        def decode(data):
                            return data.decode('utf-8', errors='replace') if isinstance(data, bytes) else data or ''
                        result = subprocess.CompletedProcess(command, 124, decode(error.stdout),
                                                             f'Timeout after {timeout} seconds\n' + decode(error.stderr))
                    except OSError as error:
                        result = subprocess.CompletedProcess(command, 1, '', str(error))
                    if stage == 'compile':
                        case['compile_log'] = (result.stdout + result.stderr).strip()
                    success = result.returncode == 0 and (stage == 'compile' or result.stdout == expected)
                    if not success:
                        parent = args.failure_dir or (args.output.parent if args.output else None)
                        folder = save_failure(parent, seed, profile, source, expected, result, stage)
                        case.update({'pass': False, 'stage': stage, 'returncode': result.returncode,
                                     'reproduction': str(folder.resolve())})
                        results.append(case)
                        print(f'FAIL seed {seed}, {profile}, {stage}: {folder}', file=sys.stderr)
                        return write_report(args.output, results, False)
                case.update({'pass': True, 'returncode': result.returncode, 'actual_lines': len(result.stdout.splitlines())})
                results.append(case)
    return write_report(args.output, results, True)


def write_report(output, results, passed):
    text = json.dumps({'status': 'PASS' if passed else 'FAIL', 'cases': results}, indent=2) + '\n'
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding='utf-8')
    print(text, end='')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
