#!/usr/bin/env python3
"""Run the owned lazy-query example through every available execution path."""
import argparse
import json
from pathlib import Path
import re
import subprocess

from lsp_intellisense import fixture_directory


parser = argparse.ArgumentParser()
parser.add_argument('--bolt', type=Path, required=True)
parser.add_argument('--cxx', default='c++')
args = parser.parse_args()
bolt = args.bolt.resolve()
root = Path(__file__).resolve().parents[1]
listing = subprocess.run([str(bolt), 'backends'], capture_output=True, text=True,
                         timeout=30, check=True).stdout
modes = []  # Interpreter cases run through Bolt.Unit; the CLI restricts native declarations.
if 'Fast: available' in listing:
    modes.append(('fast', ['--jit', '--backend', 'fast', '--require-native', '--no-debug-info']))
if 'LLVM: not available:' not in listing and 'LLVM: ' in listing:
    modes.append(('llvm', ['--jit', '--backend', 'llvm', '--require-native']))
    modes.append(('llvm-release', ['--jit', '--release', '--backend', 'llvm', '--require-native']))
modes.append(('cpp', ['--backend', 'cpp', '--cxx', args.cxx]))

with fixture_directory(None) as directory:
    source = Path(directory) / 'Main.bolt'
    source.write_text((root / 'examples/Iteration/src/Main.bolt').read_text(encoding='utf-8'),
                      encoding='utf-8')
    for name, options in modes:
        result = subprocess.run([str(bolt), 'run', str(source), *options, '--no-timings'],
                                capture_output=True, text=True, timeout=150)
        assert result.returncode == 0, (name, result.returncode, result.stdout, result.stderr)
        assert result.stdout.endswith('3\n4\nITERATION_OK\n'), (name, result.stdout, result.stderr)
        print('PASS owned lazy query, extension calls and custom range:', name, flush=True)

    # Reuse the semantic/interpreter/native cases to verify the C++ emitter too.
    pattern = re.compile(r'\{"([^"]+)",\s*R"\((.*?)\)",\s*"((?:\\.|[^"\\])*)",\s*"((?:\\.|[^"\\])*)"(?:,\s*(?:true|false))?\s*\}', re.S)
    for header in ['IterationCases.hpp', 'ExtensionMethodCases.hpp']:
        cases = pattern.findall((root / 'tests' / header).read_text(encoding='utf-8'))
        assert cases, 'No shared cases found in ' + header
        for name, code, expected, error in cases:
            if error:
                continue  # Rejections are checked by both unit harnesses.
            source.write_text(code, encoding='utf-8')
            result = subprocess.run([str(bolt), 'run', str(source), '--backend', 'cpp',
                                     '--cxx', args.cxx, '--no-timings'], capture_output=True,
                                    text=True, timeout=150)
            assert result.returncode == 0, (name, result.returncode, result.stdout, result.stderr)
            assert result.stdout.endswith(json.loads('"' + expected + '"')), (name, result.stdout, expected)
            print('PASS C++ shared case:', name, flush=True)

print(f'{len(modes)} execution paths passed')
