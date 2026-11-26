#!/usr/bin/env python3
"""Package a Linux or FreeBSD Bolt SDK, checking each ELF for OS and architecture."""
import argparse
import hashlib
import io
from pathlib import Path
import os
import stat
import struct
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MACHINES = {62: 'x64', 183: 'arm64', 243: 'riscv64'}
BINARIES = {'bolt': (2, 3), 'libBoltCompiler.so': (3,), 'libBoltRuntime.so': (3,),
            'BoltStart.o': (1,), 'BoltAotRuntime.o': (1,)}


def machine(header, kinds, name, system="Linux"):
    if len(header) >= 20 and header[:7] == b'\x7fELF\x02\x01\x01' and header[7] in ((9,) if system == 'FreeBSD' else (0, 3)):
        kind, architecture = struct.unpack_from('<HH', header, 16)
        if kind in kinds and architecture in MACHINES:
            return architecture
    raise ValueError(f'{name}: expected a supported 64-bit little-endian {system} ELF file')


def elf_interpreter(executable):
    """Read PT_INTERP from a little-endian ELF64 program without running it."""
    with executable.open('rb') as stream:
        size = os.fstat(stream.fileno()).st_size
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b'\x7fELF\x02\x01':
            return ''
        offset = struct.unpack_from('<Q', header, 32)[0]
        width, count = struct.unpack_from('<HH', header, 54)
        if width < 56 or offset + count * width > size:
            raise ValueError(f'Invalid ELF program headers: {executable}')
        for index in range(count):
            stream.seek(offset + index * width)
            entry = stream.read(56)
            if struct.unpack_from('<I', entry)[0] == 3:
                start = struct.unpack_from('<Q', entry, 8)[0]
                length = struct.unpack_from('<Q', entry, 32)[0]
                if not 1 < length <= 4096 or start + length > size:
                    raise ValueError(f'Invalid ELF interpreter: {executable}')
                stream.seek(start)
                return stream.read(length).rstrip(b'\0').decode('ascii')
    return ''


def verify_archive(path, architecture, system="Linux"):
    with path.open('rb') as stream:
        if stream.read(8) != b'!<arch>\n':
            raise ValueError(f'Expected a regular GNU archive: {path}')
        count = 0
        while header := stream.read(60):
            if len(header) != 60 or header[-2:] != b'`\n':
                raise ValueError(f'Invalid archive header in {path}')
            name = header[:16].decode('ascii').rstrip()
            length = int(header[48:58])
            data_at = stream.tell()
            if name not in ('/', '//', '/SYM64/'):
                if length < 20 or machine(stream.read(20), (1,), name, system) != architecture:
                    raise ValueError(f'Wrong object architecture in {path}: {name}')
                count += 1
            stream.seek(data_at + length + (length & 1))
        if not count or stream.tell() != path.stat().st_size:
            raise ValueError(f'Empty or truncated object archive: {path}')


def tree(folder, destination):
    if not folder.is_dir():
        raise ValueError(f'Missing SDK content at {folder}')
    return {f'{destination}/{file.relative_to(folder).as_posix()}': file
            for file in sorted(folder.rglob('*')) if file.is_file()}


def package(build, destination, vsix, system="Linux", llvm=None):
    binary = build / 'bin'
    expected = None
    musl = system == "Linux" and elf_interpreter(binary / 'bolt').startswith('/lib/ld-musl-')
    files = {}
    for name, kinds in BINARIES.items():
        if system == "FreeBSD" and name.endswith(".o"):
            continue  # FreeBSD uses its system crt1.o and the standalone BoltCore archive.
        if musl and name == 'BoltAotRuntime.o' and not (binary / name).exists():
            continue  # The Clang-built musl SDK deploys its shared runtime beside an AOT program.
        path = binary / name
        with path.open('rb') as stream:
            actual = machine(stream.read(20), kinds, name, system)
        if expected is None:
            expected = actual
        elif actual != expected:
            raise ValueError(f'Mixed architectures in SDK, found {MACHINES[actual]} in {name}')
        files['bin/' + name] = path
    verify_archive(binary / 'libBoltCore.a', expected, system)
    if system == 'FreeBSD':
        if expected != 62 or llvm is None:
            raise ValueError('FreeBSD packaging requires x86-64 binaries and --llvm-dir')
        for name in ('libLLVM.so.19.1', 'libzstd.so.1'):
            path = llvm / 'lib' / name
            with path.open('rb') as stream:
                if machine(stream.read(20), (3,), name, system) != expected:
                    raise ValueError(f'Wrong LLVM architecture: {path}')
        files.update(tree(llvm, 'bin/llvm'))
    files['bin/libBoltCore.a'] = binary / 'libBoltCore.a'
    files['bin/bolt-watch.py'] = ROOT / 'tools/bolt-watch.py' if system == 'FreeBSD' else binary / 'bolt-watch.py'
    files.update(tree(ROOT / 'include' if system == 'FreeBSD' else binary / 'include', 'bin/include'))
    if system == 'Linux' and expected in (62, 183):
        example = Path('examples/LinuxDebug')
        for name in ('README.md', 'bolt.proj', 'src/Main.bolt', '.vscode/launch.json', '.vscode/tasks.json'):
            files[(example / name).as_posix()] = ROOT / example / name
    for required in ('bin/include/bolt/CompilerHost.h', 'bin/include/bolt/Bolt.hpp'):
        if required not in files:
            raise ValueError(f'Missing SDK header: {required}')
    files.update(tree(ROOT / 'stdlib', 'share/bolt/stdlib'))
    files.update(tree(ROOT / 'docs/licenses', 'share/bolt/docs/licenses'))
    files.update(tree(ROOT / 'examples/FunkeBolt', 'examples/FunkeBolt'))
    for name in ('setup.sh', 'uninstall.sh', 'sdk-setup.py', 'SETUP.md'):
        files[name] = ROOT / 'tools/sdk-setup' / name
    files['share/bolt/LICENSE'] = ROOT / 'LICENSE'
    files['share/bolt/THIRD_PARTY_NOTICES.md'] = ROOT / 'THIRD_PARTY_NOTICES.md'
    files['share/bolt/docs/ThirdParty.md'] = ROOT / 'docs/ThirdParty.md'
    files['Watcher.md'] = ROOT / 'docs/Watcher.md'
    if vsix is not None:
        files['Bolt-VSCode.vsix'] = vsix
    arch = MACHINES[expected]
    variant = system + ('-musl' if musl else '')
    doc = 'Linux-musl-Desktop.md' if musl else f'{system}-SDK.md'
    files['README.md'] = (f'# Bolt für {variant} {arch}\n\n'.encode() + (ROOT / 'docs' / doc).read_bytes().split(b'\n', 1)[1].lstrip())
    prefix = f'Bolt-{variant}-{arch}/'
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temp:
        scratch = Path(temp.name)
    checksums = []
    try:
        with zipfile.ZipFile(scratch, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            def write(name, source):
                # Fixed ZIP timestamps make a second package of the same build reproducible.
                info = zipfile.ZipInfo(prefix + name)
                mode = 0o755 if name in ('bin/bolt', 'bin/bolt-watch.py', 'setup.sh', 'uninstall.sh') else 0o644
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                digest = hashlib.sha256()
                with (io.BytesIO(source) if isinstance(source, bytes) else source.open('rb')) as stream:
                    info.file_size = len(source) if isinstance(source, bytes) else os.fstat(stream.fileno()).st_size
                    with archive.open(info, 'w') as member:
                        while block := stream.read(1024 * 1024):
                            member.write(block)
                            digest.update(block)
                return digest.hexdigest()
            for name, source in sorted(files.items()):
                checksums.append(f'{write(name, source)}  {name}\n')
            write('SHA256SUMS', ''.join(checksums).encode())
        scratch.chmod(0o644)
        os.replace(scratch, destination)
    finally:
        scratch.unlink(missing_ok=True)
    print(f'Packaged {prefix[:-1]} ({len(checksums)} files): {destination}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path, help='Build directory containing the bin/ SDK')
    parser.add_argument('output', type=Path, help='Destination .zip file')
    parser.add_argument('--vsix', type=Path, help='Include the matching, prebuilt Bolt VS Code extension')
    parser.add_argument('--system', choices=('Linux', 'FreeBSD'), default='Linux')
    parser.add_argument('--llvm-dir', type=Path, help='Staged FreeBSD LLVM distribution and notices')
    args = parser.parse_args()
    try:
        package(args.build, args.output, args.vsix, args.system, args.llvm_dir)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
