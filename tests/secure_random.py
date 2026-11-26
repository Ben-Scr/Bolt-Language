#!/usr/bin/env python3
"""Check the public CSPRNG on the installed native and interpreted execution paths."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bolt', type=Path, required=True)
    args = p.parse_args()
    bolt = args.bolt.resolve()
    backends = subprocess.check_output([bolt, 'backends'], text=True)
    with tempfile.TemporaryDirectory(prefix='Bolt secure random ') as temporary:
        root = Path(temporary)
        script = root / 'Random.bolt'
        shutil.copyfile(Path(__file__).with_name('fixtures') / 'secure-random.bolt', script)
        subprocess.run([bolt, 'run', script, '--runtime'], cwd=root, check=True)
        for backend, valid in [('fast', 'Fast: available' in backends),
                               ('llvm', 'LLVM: not available' not in backends)]:
            if valid:
                subprocess.run([bolt, 'run', script, '--jit', '--backend', backend], cwd=root, check=True)
                subprocess.run([bolt, 'run', script, '--backend', backend, '--require-native', '--no-debug-info'],
                               cwd=root, check=True)
        if 'LLVM: not available' not in backends:
            for profile in ('--debug', '--release'):
                subprocess.run([bolt, 'run', script, profile, '--no-runtime'], cwd=root, check=True)
    print('PASS shared and owned random sequences, OS entropy, buffer filling and invalid input')


if __name__ == '__main__':
    main()
