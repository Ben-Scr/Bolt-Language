#!/usr/bin/env python3
"""Compare unchanged Bolt memory workloads to GCC with the same facts expressed by hand."""
import argparse
import json
import math
import random
import statistics
import tempfile
from pathlib import Path

from compare import executable_path, sha256_file
from suite import ROOT, CASES, captured, cpu_info


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"expected one occurrence of {old!r} in the versioned fixture")
    return source.replace(old, new)


def measure(args, root):
    compilers = {k: str(executable_path(v)) for k, v in
                 [('c', args.cc), ('bolt', args.bolt)]}
    if args.previous:
        compilers['previous-bolt'] = str(executable_path(args.previous))
    directory = root.resolve()
    fixture = 'memory' if args.case == 'memory-long' else args.case
    inputs = {lang: (ROOT / 'cases' / (fixture + '.' + ext)).read_text()
              for lang, ext in [('bolt', 'bolt'), ('c', 'c')]}
    if args.case == 'memory-long':
        inputs = {k: replace_once(v, 'step < 250000;', 'step < 2000000;') for k, v in inputs.items()}
        inputs['c-proven-prefix'] = replace_once(inputs['c'], 'memcmp(a, b, n)',
                                                '(int)a[n - 1] - (int)b[n - 1]')
    else:
        # Every table entry is in [0, 65535], as is the initial cursor. GCC
        # can use a zero-extending 32-bit load and omit both runtime guards.
        text = replace_once(inputs['c'], 'int32_t *links', 'uint32_t *links')
        text = replace_once(text, 'int32_t cursor =', 'uint32_t cursor =')
        text = replace_once(text, '        if ((uint32_t)cursor >= 65536) return 2;\n', '')
        inputs['c-proven-indices'] = replace_once(text, '        if (cursor < 0) return 3;\n', '')
    if args.previous:
        inputs['previous-bolt'] = inputs['bolt']
    report = {
        'machine': cpu_info(), 'expected_stdout': CASES[args.case], 'case': args.case,
        'metric': 'whole-process wall time in milliseconds',
        'compilers': {k: {'sha256': sha256_file(Path(exe)), 'version': captured(
            [exe, '--version' if k == 'c' else 'version'], directory, 60)[1]}
            for k, exe in compilers.items()},
        'warmup': args.warmup, 'runs': args.runs, 'seed': args.seed, 'profiles': {},
    }
    executables = {}
    for name, text in inputs.items():
        is_c = name == 'c' or name.startswith('c-')
        folder = directory / name
        folder.mkdir()
        file = folder / ('program.c' if is_c else 'program.bolt')
        exe = folder / 'program'
        file.write_text(text)
        command = [compilers['c'], '-std=c11', '-O3', '-DNDEBUG', file.name, '-o', exe.name] if is_c else [
            compilers[name], 'compile', file.name, '--release', '--backend', 'llvm', '--require-native', '-o', exe.name]
        _, log = captured(command, folder, 120)
        executables[name] = exe
        report['profiles'][name] = {'source_sha256': sha256_file(file), 'command': command, 'build_log': log,
                                    'executable_sha256': sha256_file(exe), 'samples_ms': [],
                                    'runtime_sha256': {p.name: sha256_file(p) for p in folder.glob('*BoltRuntime*')}}
    report['order'] = []
    rng = random.Random(args.seed)
    for i in range(args.warmup + args.runs):
        order = list(inputs)
        rng.shuffle(order)
        report['order'].append(order)
        for name in order:
            exe = executables[name]
            duration, output = captured([str(exe)], exe.parent, 120)
            if output != report['expected_stdout']:
                raise ValueError(f'{name} returned {output!r}')
            if i >= args.warmup:
                report['profiles'][name]['samples_ms'].append(round(duration / 1e6, 4))
    for profile in report['profiles'].values():
        samples = profile['samples_ms']
        profile['p50_ms'] = statistics.median(samples)
        profile['p95_ms'] = sorted(samples)[math.ceil(len(samples) * .95) - 1]
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', default='bolt')
    parser.add_argument('--previous', help='Optional previous Bolt executable')
    parser.add_argument('--case', choices=['memory-long', 'indirect-array'], default='memory-long')
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--runs', type=int, default=31)
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--seed', type=int, default=451786)
    parser.add_argument('--json', type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0:
        parser.error('invalid run count')
    with tempfile.TemporaryDirectory() as d:
        result = measure(args, Path(d))
    args.json.write_text(json.dumps(result, indent=2) + '\n')
    for name, values in result['profiles'].items():
        print(f"{name:16} p50 {values['p50_ms']:.3f} ms  p95 {values['p95_ms']:.3f} ms")
