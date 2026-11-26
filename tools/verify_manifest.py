#!/usr/bin/env python3
"""Check the SHA-256 file list of an extracted Bolt source tree or release bundle."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re
import sys


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def verify(root: Path, strict: bool) -> int:
    root = root.resolve(strict=True)
    manifest = root / 'SOURCE_MANIFEST.sha256'
    kind = 'source'
    if not manifest.is_file() and (root / 'SHA256SUMS').is_file():
        manifest = root / 'SHA256SUMS'
        kind = 'package'
    if not manifest.is_file():
        raise ValueError('No Bolt checksum list in ' + str(root) + '. Extract the complete release first.')
    expected: set[str] = set()
    for number, line in enumerate(manifest.read_text(encoding='utf-8').splitlines(), 1):
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        if not match:
            raise ValueError(f'Invalid manifest record at line {number}')
        wanted, relative = match.groups()
        path = PurePosixPath(relative)
        if path.is_absolute() or '..' in path.parts or '\\' in relative or ':' in relative:
            raise ValueError(f'Unsafe path at line {number}: {relative}')
        if relative in expected or relative == manifest.name:
            raise ValueError(f'Duplicate/self-referencing path: {relative}')
        expected.add(relative)
        candidate = root.joinpath(*path.parts)
        if any(parent.is_symlink() for parent in (candidate, *candidate.parents) if parent != root):
            raise ValueError(f'Symlink is not a delivered regular file: {relative}')
        if not candidate.is_file() or digest(candidate) != wanted:
            raise ValueError(f'Missing or changed file: {relative}')
    if not expected:
        raise ValueError('The checksum list is empty: ' + manifest.name)
    if strict:
        actual = {path.relative_to(root).as_posix() for path in root.rglob('*')
                  if path.is_file() and path != manifest}
        extra = actual - expected
        if extra:
            raise ValueError('Unlisted files: ' + ', '.join(sorted(extra)))
    print(f'PASS: {len(expected)} {kind} files match SHA-256 manifest'
          + ('; exact file coverage' if strict else ''))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    script = Path(__file__).resolve()
    default_root = script.parent if script.name == 'verify.py' else script.parents[1]
    parser.add_argument('root', nargs='?', type=Path, default=default_root)
    parser.add_argument('--strict', action='store_true',
                        help='Also reject extra files; use on an untouched extracted archive')
    args = parser.parse_args()
    try:
        return verify(args.root, args.strict)
    except (OSError, ValueError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
