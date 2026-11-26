#!/usr/bin/env python3
"""End-to-end project, namespace, cache, debug/release, and failure checks."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    parser.add_argument('--cxx', required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    bolt = str(args.bolt.resolve())
    work = args.work.resolve()
    project = work / 'Folder with spaces'
    if project.exists():
        shutil.rmtree(project)
    shutil.copytree(ROOT / 'examples' / 'FeatureProject', project,
                    ignore=shutil.ignore_patterns('bin', '.bolt', '*.bolt.cpp'))
    expected = (project / 'expected.txt').read_text(encoding='utf-8')
    count = 0
    def execute(command: list[str], success: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(command, text=True, encoding='utf-8', errors='replace',
                                capture_output=True, timeout=120)
        if (result.returncode == 0) != success:
            raise AssertionError(f'{command}\n{result.stdout}\n{result.stderr}')
        return result
    def passed(name: str) -> None:
        nonlocal count
        count += 1
        print('PASS ' + name, flush=True)
    execute([bolt, 'check', str(project)])
    passed('folder imports and namespaced generic concept')
    assert execute([bolt, 'run', str(project), '--runtime']).stdout == expected
    passed('folder interpreter output')
    suffix = '.exe' if os.name == 'nt' else ''
    outputs = {}
    for mode in ('debug', 'release'):
        result = execute([bolt, 'compile', str(project), '--' + mode, '--cxx', args.cxx])
        target = project / 'bin' / mode.title() / (project.name + suffix)
        assert target.is_file(), result.stdout
        assert execute([str(target)]).stdout == expected
        outputs[mode] = target
        passed(mode + ' default output and native execution')
        result = execute([bolt, 'compile', str(project), '--' + mode, '--cxx', args.cxx])
        assert 'Up to date' in result.stdout, result.stdout
        passed(mode + ' no-change cache')
    source = project / 'Program.bolt'
    original = source.read_text(encoding='utf-8')
    source.write_text(original.replace('int value = 3;', 'int value = 4;'), encoding='utf-8')
    result = execute([bolt, 'compile', str(project), '--debug', '--cxx', args.cxx])
    assert 'Built Debug' in result.stdout, result.stdout
    changed = expected.replace('10, 5\n8\n', '10, 5\n9\n', 1)
    assert execute([str(outputs['debug'])]).stdout == changed
    passed('source edit invalidates native cache')
    before = outputs['debug'].read_bytes()
    source.write_text(original + '\nclass Invalid : MissingType {}\n', encoding='utf-8')
    execute([bolt, 'compile', str(project), '--debug', '--cxx', args.cxx], success=False)
    assert outputs['debug'].read_bytes() == before
    passed('failed frontend preserves previous executable')
    source.write_text(original, encoding='utf-8')
    execute([bolt, 'compile', str(project), '--debug', '--cxx', str(work / 'missing-compiler')], success=False)
    assert outputs['debug'].read_bytes() == before
    passed('failed native tool preserves previous executable')
    result = execute([bolt, 'run', str(project), '--native', '--release', '--cxx', args.cxx])
    assert result.stdout.endswith(expected), result.stdout
    passed('run --native compiles and launches')
    execute([bolt, 'compile', str(project), '--debug', '--release'], success=False)
    passed('conflicting modes rejected')
    print(f'{count} project/CLI checks passed')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
