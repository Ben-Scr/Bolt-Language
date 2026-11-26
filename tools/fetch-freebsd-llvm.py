#!/usr/bin/env python3
"""Extract the pinned FreeBSD/amd64 LLVM runtime and its zstd dependency."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen


def stage(output, cache):
    cache.mkdir(parents=True, exist_ok=True)
    lock = Path(__file__).with_name('freebsd-llvm.lock.json')
    for name, package in json.loads(lock.read_text()).items():
        path = cache / package['url'].rsplit('/', 1)[-1]
        if not path.exists():
            with tempfile.NamedTemporaryFile(dir=cache, delete=False) as temp:
                temporary = Path(temp.name)
                try:
                    with urlopen(package['url'], timeout=90) as download:
                        shutil.copyfileobj(download, temp)
                    temp.flush()
                    if digest(temporary) != package['sha256']:
                        raise ValueError(f'{name}: package SHA-256 mismatch')
                    temporary.replace(path)
                finally:
                    temporary.unlink(missing_ok=True)
        if digest(path) != package['sha256']:
            raise ValueError(f'{path}: cached package SHA-256 mismatch')
        members = {package['member']: output / 'lib' / package['output'],
                   '+MANIFEST': output / 'share/provenance' / (name + '.json')}
        prefix = f"/usr/local/share/licenses/{name}-{package['version']}/"
        members.update({prefix + file: output / 'share/licenses' / name / file for file in package['notices']})
        # Python before 3.14 requires an explicit reader for zstd archives.
        with path.open('rb') as data:
            magic = data.read(4)
            data.seek(0)
            if magic == b'\x28\xb5\x2f\xfd':
                try:
                    import zstandard
                except ImportError:
                    with tempfile.TemporaryFile() as expanded:
                        subprocess.run(['zstd', '-dc', str(path)], check=True, stdout=expanded)
                        expanded.seek(0)
                        unpack(expanded, members)
                else:
                    with zstandard.ZstdDecompressor().stream_reader(data) as reader:
                        unpack(reader, members)
            else:
                unpack(data, members)
        if members:
            raise ValueError(f'{name}: missing package members: {list(members)}')
    shutil.copy2(lock, output / 'share/provenance' / lock.name)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def unpack(stream, members):
    with tarfile.open(fileobj=stream, mode='r|*') as archive:
        for member in archive:
            target = members.get(member.name)
            if target is None:
                continue
            if not member.isfile():
                raise ValueError(f'Expected regular file: {member.name}')
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('wb') as result:
                shutil.copyfileobj(archive.extractfile(member), result)
            del members[member.name]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--cache', type=Path, default=Path.home() / '.cache' / 'bolt-freebsd')
    args = parser.parse_args()
    stage(args.destination, args.cache)
