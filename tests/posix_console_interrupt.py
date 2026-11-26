#!/usr/bin/env python3
"""Interrupt native console reads and writes while a pipe is blocked."""
import argparse
from pathlib import Path
import select
import signal
import subprocess
import tempfile
import time

FIXTURE = Path(__file__).resolve().parent / 'fixtures/posix-console-signal.bolt'


def interrupt(process):
    # A full output pipe may first return a positive partial write. Further
    # signals then interrupt the subsequent write before the reader drains it.
    for _ in range(4):
        time.sleep(0.075)
        process.send_signal(signal.SIGUSR1)


def exercise(executable):
    with subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, bufsize=0) as child:
        try:
            assert select.select([child.stdout], [], [], 60)[0] and child.stdout.readline(16) == b'READ\n'
            interrupt(child)
            child.stdin.write(b'scene\n')
            child.stdin.flush()
            assert select.select([child.stdout], [], [], 60)[0] and child.stdout.readline(16) == b'WRITE\n'
            interrupt(child)
            output, errors = child.communicate(timeout=60)
            assert child.returncode == 0 and not errors, (child.returncode, errors)
            assert output == b'Bolt' * (1 << 18), len(output)
        finally:
            if child.poll() is None:
                child.kill()
    print('PASS interrupted POSIX input and output, including a 1 MiB pipe write')


if __name__ == '__main__':
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--bolt', required=True, type=Path)
    bolt = args.parse_args().bolt.resolve()
    with tempfile.TemporaryDirectory(prefix='Bolt interrupted console ') as directory:
        exe = Path(directory) / 'console'
        subprocess.run([bolt, 'compile', FIXTURE, '--backend', 'cpp', '--no-pch',
                        '--no-debug-info', '-o', exe], check=True)
        exercise(exe)
