#!/usr/bin/env python3
"""Stage SHA-256-pinned LLVM 19, LLD, their shared dependencies and notices for a macOS SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen
import zipfile

COMPONENTS = {
    'libllvm19': ('lib/libLLVM.19.1.dylib', 'lib/libLLVM.19.1.dylib'),
    'lld': ('bin/lld', 'bin/ld64.lld'),
    'libcxx': ('lib/libc++.1.dylib', 'lib/libc++.1.dylib'),
    'libzlib': ('lib/libz.1.dylib', 'lib/libz.1.dylib'),
    'zstd': ('lib/libzstd.1.dylib', 'lib/libzstd.1.dylib'),
    'libxml2-16': ('lib/libxml2.16.dylib', 'lib/libxml2.16.dylib'),
    'libiconv': ('lib/libiconv.2.dylib', 'lib/libiconv.2.dylib'),
    'liblzma': ('lib/liblzma.5.dylib', 'lib/liblzma.5.dylib'),
}


def sha256(path):
    with path.open('rb') as f:
        digest = hashlib.sha256()
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
        return digest.hexdigest()


def cached(cache, url, digest):
    path = cache / url.rsplit('/', 1)[1]
    if not path.exists():
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as tmp:
            temporary = Path(tmp.name)
            try:
                with urlopen(url, timeout=90) as input_file:
                    shutil.copyfileobj(input_file, tmp)
                tmp.flush()
                if sha256(temporary) != digest:
                    raise ValueError(f'Download checksum mismatch: {url}')
                os.replace(temporary, path)
            finally:
                temporary.unlink(missing_ok=True)
    if sha256(path) != digest:
        raise ValueError(f'Cache checksum mismatch: {path}. Remove the file and retry.')
    return path


def extract(package, root, component, member_path, installed):
    try:
        import zstandard
    except ImportError:
        raise SystemExit('The SDK builder needs Python zstandard to read .conda archives.')
    payload = root / installed
    # Package symlink chains are resolved only within the library's own directory.
    # Read the small matching family of SONAMEs to handle a symlink preceding or
    # following its real library in a streaming tar archive.
    folder, filename = member_path.rsplit('/', 1)
    stem = filename.split('.')[0]
    files, links = {}, {}
    with tempfile.TemporaryDirectory(prefix='bolt-conda-') as tmp, zipfile.ZipFile(package) as z:
        temporary = Path(tmp)
        for name in z.namelist():
            if not name.endswith('.tar.zst'):
                continue
            with z.open(name) as packed, zstandard.ZstdDecompressor().stream_reader(packed) as stream, tarfile.open(fileobj=stream, mode='r|') as tar:
                for item in tar:
                    p = PurePosixPath(item.name)
                    if p.is_absolute() or '..' in p.parts:
                        raise ValueError(f'Invalid package member: {item.name}')
                    if str(p.parent) == folder and (p.name == filename or p.name.startswith(stem+'.')):
                        if item.issym():
                            to = PurePosixPath(item.linkname)
                            if len(to.parts) != 1 or to.name in ('.', '..'):
                                raise ValueError(f'Invalid library link: {item.linkname}')
                            links[p.name] = to.name
                        elif item.isfile():
                            target = temporary / p.name
                            with target.open('wb') as out:
                                shutil.copyfileobj(tar.extractfile(item), out)
                            files[p.name] = target
                    elif item.isfile() and (p.parts[:2] == ('info', 'licenses') or
                          (component == 'libiconv' and p.parts[:2] == ('info', 'recipe'))):
                        target = root / 'share' / ('licenses' if p.parts[1] == 'licenses' else 'sources') / component
                        target = target.joinpath(*p.parts[2:])
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with target.open('wb') as out:
                            shutil.copyfileobj(tar.extractfile(item), out)
        current = filename
        seen = set()
        while current in links and current not in seen:
            seen.add(current)
            current = links[current]
        if current not in files:
            raise ValueError(f'{member_path} is missing or cyclic in {package.name}')
        payload.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(files[current], payload)
        payload.chmod(0o755)
    if not any((root/'share/licenses'/component).rglob('*')):
        raise ValueError(f'Missing license text for {component}')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--arch', choices=('x86_64','arm64'), required=True)
    p.add_argument('--cache', type=Path, required=True, help='Downloaded, SHA-256-checked package cache')
    p.add_argument('--output', type=Path, required=True, help='New directory with the ready-to-package toolchain')
    p.add_argument('--zig', type=Path, required=True, help='Zig 0.14.1 supplying its Darwin libSystem link stub')
    args = p.parse_args()
    lock_path = Path(__file__).with_name('macos-llvm.lock.json')
    lock = json.loads(lock_path.read_text())
    zig = args.zig.resolve()
    if subprocess.check_output([zig, 'version'], text=True).strip() != '0.14.1':
        p.error('Use Zig 0.14.1 for its known Darwin system link declarations')
    lib = Path(json.loads(subprocess.check_output([zig,'env'],text=True))['lib_dir']).resolve()
    args.cache.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    if output.exists():
        p.error('Choose a new output directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix='bolt-macos-tools-') as temp:
        staged = Path(temp) / 'llvm'
        for component, (member,dest) in COMPONENTS.items():
            record = lock[args.arch][component]
            url = 'https://conda.anaconda.org/conda-forge/' + record['path']
            package = cached(args.cache, url, record['sha256'])
            extract(package, staged, component, member, dest)
        src = lock['libiconv-source']
        archive = cached(args.cache, src['url'], src['sha256'])
        sources = staged/'share/sources/libiconv'
        sources.mkdir(parents=True, exist_ok=True)
        shutil.copy2(archive, sources/archive.name)
        for name, location in [('SDKSettings.json','SDKSettings.json'),('libSystem.tbd','usr/lib/libSystem.tbd')]:
            source = lib/'libc/darwin'/name
            if sha256(source) != lock['zig-darwin'][name]:
                raise ValueError(f'Unknown SDK link declaration: {source}')
            dest = staged/'sysroot'/location
            dest.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(source,dest)
        shutil.copy2(lock_path, staged/'share/provenance.json')
        shutil.copy2(lib.parent/'LICENSE', staged/'share/licenses/Zig.txt')
        staged.rename(output)
    print(output)


if __name__ == '__main__':
    main()
