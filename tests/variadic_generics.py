#!/usr/bin/env python3
"""Exercise typed packs through source libraries and reflected calls."""
import argparse
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--bolt', type=Path, required=True)
parser.add_argument('--cxx', default='c++')
args = parser.parse_args()
root = Path(__file__).resolve().parent
bolt = args.bolt.resolve()

def check(fixture, options, expected):
    result = subprocess.run([str(bolt), 'run', str(root / 'features' / fixture),
                             *options, '--no-timings'], text=True,
                            capture_output=True, timeout=150)
    assert result.returncode == 0, (fixture, options, result.stdout, result.stderr)
    # Build progress is intentionally outside the program output suffix.
    assert result.stdout.endswith(expected), (fixture, result.stdout, expected)
    print('PASS', fixture, ' '.join(options), flush=True)

core = '15\n13\n16\n9\nref!\n9\n34\n34\n52\ntrue\ntrue\ntrue\n'
check('heterogeneous-packs.bolt', ['--runtime'], core)
cpp = ['--backend', 'cpp', '--cxx', args.cxx]
check('heterogeneous-packs.bolt', cpp, core)
backends = subprocess.check_output([str(bolt), 'backends'], text=True)
reflection_modes = [cpp]
if 'Fast: available' in backends:
    reflection_modes.append(['--jit', '--backend', 'fast', '--require-native'])
if 'LLVM: ' in backends and 'LLVM: not available' not in backends:
    reflection_modes.append(['--release', '--backend', 'llvm', '--require-native'])
for mode in reflection_modes:
    check('variadic-reflection.bolt', mode, '4\n12\n7\n7\nref!\n18\n')
