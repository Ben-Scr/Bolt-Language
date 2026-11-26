#!/usr/bin/env python3
"""Install a prebuilt Linux, FreeBSD or macOS Bolt SDK for one user."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
MARKER = '.bolt-portable-install.json'
PRODUCT = 'bolt-portable-sdk-v1'
EXTENSION = 'ben-schneider.bolt-language'
HELPERS = ('setup.sh', 'uninstall.sh', 'sdk-setup.py', 'SETUP.md')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def local(root, name):
    if not name or any(s in ('', '.', '..') for s in name.split('/')) or '\\' in name:
        raise ValueError(f'Invalid SDK file name: {name}')
    target = root / name
    if target.is_symlink() or any(p.is_symlink() for p in target.parents):
        raise ValueError(f'Symbolic link in SDK path: {target}')
    return target


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as f:
        temp = Path(f.name)
    try:
        temp.write_bytes(data)
        temp.chmod(path.stat().st_mode & 0o777 if path.exists() else 0o600)
        temp.replace(path)
    finally:
        temp.unlink(missing_ok=True)


def record(folder, state):
    save(folder / MARKER, json.dumps(state, indent=2).encode())


def load(prefix):
    state = json.loads(local(prefix, MARKER).read_text())
    if state.get('product') != PRODUCT or state.get('prefix') != str(prefix):
        raise ValueError(f'Unmanaged installation: {prefix}')
    for name in state['files']:
        local(prefix, name)
    return state


def macos_default_prefix(home):
    primary = home / '.local/share/bolt'
    alternate = primary.with_name('bolt-sdk')

    def owned(folder, name, product):
        marker = folder / name
        if folder.is_symlink() or marker.is_symlink() or not marker.is_file():
            return {}
        try:
            state = json.loads(marker.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return {}
        if (isinstance(state, dict) and state.get('product') == product
                and state.get('prefix') == str(folder.resolve())
                and isinstance(state.get('files'), dict) and 'bin/bolt' in state['files']):
            return state
        return {}

    if owned(primary, MARKER, PRODUCT):
        return primary, None
    source = str(primary.resolve())
    sibling = owned(alternate, MARKER, PRODUCT)
    if sibling.get('default_source_prefix') == source:
        return alternate, source
    if owned(primary, '.bolt-install.json', 'bolt-user-install-v1'):
        if sibling:
            raise ValueError(f'A separate SDK already uses {alternate}. Choose an explicit --prefix.')
        return alternate, source
    return primary, None


def macho_target(stream, start, size):
    stream.seek(start)
    header = stream.read(32)
    if len(header) != 32 or header[:4] != b'\xcf\xfa\xed\xfe':
        return None
    machine, _, kind, count, length = struct.unpack_from('<IIIII', header, 4)
    cpus = {0x01000007: 'x86_64', 0x0100000C: 'arm64'}
    if kind != 2 or machine not in cpus or length > size - 32:
        return None
    # Read load-command headers individually; skip unrelated commands.
    offset = start + 32
    end = offset + length
    minimum = None
    for _ in range(count):
        if offset + 8 > end:
            return None
        stream.seek(offset)
        cmd, length = struct.unpack('<II', stream.read(8))
        if length < 8 or offset + length > end:
            return None
        if cmd == 0x32 and length >= 24:  # LC_BUILD_VERSION
            target, minimum = struct.unpack('<II', stream.read(8))
            if target != 1:                # macOS, distinct from iOS/simulator
                return None
        elif cmd == 0x24 and length >= 16:  # LC_VERSION_MIN_MACOSX
            minimum = struct.unpack('<I', stream.read(4))[0]
        offset += length
    return (cpus[machine], minimum) if minimum is not None else None


def freebsd_abi(stream, size):
    """Read NT_FREEBSD_ABI_TAG from a bounded ELF64 PT_NOTE segment."""
    stream.seek(0)
    elf = stream.read(64)
    if len(elf) != 64:
        raise ValueError('Truncated FreeBSD ELF header')
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    step, count = struct.unpack_from('<HH', elf, 54)
    if step < 56 or count > 1024 or phoff + step * count > size:
        raise ValueError('Invalid FreeBSD ELF program header table')
    for index in range(count):
        stream.seek(phoff + index * step)
        segment = stream.read(56)
        if struct.unpack_from('<I', segment)[0] != 4:
            continue
        offset, length = struct.unpack_from('<Q', segment, 8)[0], struct.unpack_from('<Q', segment,32)[0]
        if length > 1024 * 1024 or offset + length > size:
            raise ValueError('Invalid FreeBSD ELF notes')
        stream.seek(offset)
        notes = stream.read(length)
        ptr = 0
        while ptr + 12 <= len(notes):
            namesz, descsz, kind = struct.unpack_from('<III', notes, ptr)
            start = ptr + 12
            desc = (start + namesz + 3) & ~3
            ptr = (desc + descsz + 3) & ~3
            if ptr > len(notes):
                raise ValueError('Truncated FreeBSD ELF note')
            if kind == 1 and notes[start:start+namesz] == b'FreeBSD\0' and descsz == 4:
                return struct.unpack_from('<I', notes, desc)[0]
    return None


def sdk_target(executable):
    """Return the operating system and minimum version per native architecture."""
    with executable.open('rb') as stream:
        size = os.fstat(stream.fileno()).st_size
        header = stream.read(32)
        if len(header) >= 20 and header[:7] == b'\x7fELF\x02\x01\x01' and header[7] in (0, 3, 9):
            kind, machine = struct.unpack_from('<HH', header, 16)
            if kind in (2, 3) and header[7] == 9:
                if machine == 62:
                    return 'FreeBSD', {'x86_64': freebsd_abi(stream,size)}
                raise ValueError('Unsupported FreeBSD SDK architecture')
            if kind in (2, 3) and machine in (62, 183, 243):
                cpu = {62: 'x86_64', 183: 'arm64', 243: 'riscv64'}[machine]
                return 'Linux', {cpu: None}
        thin = macho_target(stream, 0, size)
        if thin:
            return 'Darwin', dict([thin])
        if header[:4] in (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf') and len(header) >= 8:
            count = struct.unpack_from('>I', header, 4)[0]
            width = 20 if header[3] == 0xbe else 32
            if 0 < count <= 128 and size >= 8 + count * width:
                stream.seek(8)
                records = stream.read(count * width)
                slices = {}
                for off in range(0, len(records), width):
                    machine = struct.unpack_from('>I', records, off)[0]
                    start, length = struct.unpack_from('>II' if width == 20 else '>QQ', records, off+8)
                    if start > size or length > size - start:
                        raise ValueError('Truncated universal macOS executable')
                    item = macho_target(stream, start, length)
                    if item and machine == {'arm64': 0x0100000C, 'x86_64': 0x01000007}[item[0]]:
                        slices[item[0]] = item[1]
                if slices:
                    return 'Darwin', slices
    raise ValueError(f'Unrecognized Linux, macOS or FreeBSD 64-bit Bolt executable: {executable}')


def compatible_cpu(system, cpu, available):
    if cpu in available:
        return cpu
    if system == 'Darwin':
        if cpu == 'x86_64' and 'arm64' in available:
            # A translated Python process can still install native Apple Silicon binaries.
            test = subprocess.run(['/usr/sbin/sysctl', '-in', 'sysctl.proc_translated'], capture_output=True, text=True)
            if test.returncode == 0 and test.stdout.strip() == '1':
                return 'arm64'
        if cpu == 'arm64' and 'x86_64' in available:
            test = subprocess.run(['/usr/bin/arch', '-x86_64', '/usr/bin/true'], capture_output=True)
            if test.returncode == 0:
                return 'x86_64'
    return None


def require_host(executable):
    system, architectures = sdk_target(executable)
    host, cpu = platform.system(), platform.machine().lower()
    cpu = {'aarch64': 'arm64', 'amd64': 'x86_64'}.get(cpu, cpu)
    match = compatible_cpu(host, cpu, architectures) if host == system else None
    if not match:
        packaged = '/'.join(sorted(architectures))
        raise ValueError(f'This SDK targets {system} {packaged}. Choose the Bolt SDK for {host} {cpu}.')
    if system == 'FreeBSD' and architectures[match] is not None:
        abi = architectures[match]
        required = (abi // 100000, (abi // 1000) % 100)
        installed = re.match(r'([0-9]+)\.([0-9]+)', platform.release())
        if installed and tuple(map(int, installed.groups())) < required:
            raise ValueError(f'This Bolt SDK requires FreeBSD {required[0]}.{required[1]} or newer')
    if system == 'Darwin':
        v = architectures[match]
        minimum = (v >> 16, (v >> 8) & 255, v & 255)
        installed = platform.mac_ver()[0]
        numbers = [int(x) for x in installed.split('.') if x.isdecimal()]
        if numbers and tuple((numbers + [0, 0])[:3]) < minimum:
            required = '.'.join(map(str, minimum))
            raise ValueError(f'This Bolt SDK requires macOS {required} or newer; this Mac runs {installed}.')


def payload(source):
    sums = local(source, 'SHA256SUMS')
    files = {}
    for line in sums.read_text().splitlines():
        m = re.fullmatch(r'([a-fA-F0-9]{64})  (.+)', line)
        if not m:
            raise ValueError('Invalid SHA256SUMS entry')
        checksum, name = m.groups()
        if name in files or name in (MARKER, 'SHA256SUMS'):
            raise ValueError(f'Invalid duplicate or reserved entry: {name}')
        files[name] = (local(source, name), checksum.lower())
    if 'bin/bolt' not in files:
        raise ValueError('Missing bin/bolt. Use --source pointing to the extracted Linux, FreeBSD or macOS SDK.')
    require_host(files['bin/bolt'][0])
    files['SHA256SUMS'] = (sums, digest(sums))
    for name in HELPERS:
        if name not in files:
            helper = HERE / name
            files[name] = (helper, digest(helper))
    return files


def install_files(prefix, source, old):
    files = payload(source)
    prior = old['files']
    work = Path(tempfile.mkdtemp(prefix='.bolt-sdk-', dir=prefix.parent))
    backup = work / 'old'
    committed = False
    try:
        stage = work / 'new'
        stage.mkdir()
        # Preserve unknown files and directories on upgrade. Fail before the swap on collisions.
        if prefix.exists():
            for p in prefix.rglob('*'):
                name = p.relative_to(prefix).as_posix()
                local(prefix, name)
                if name == MARKER:
                    continue
                dest = stage / name
                if p.is_dir():
                    dest.mkdir(parents=True, exist_ok=True)
                elif not p.is_file():
                    raise ValueError(f'Unsupported file in installation: {p}')
                elif name in prior:
                    if digest(p) != prior[name]:
                        raise ValueError(f'Modified SDK file; move it before updating: {p}')
                elif name in files:
                    raise ValueError(f'Would replace an unowned file: {p}')
                else:
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    os.link(p, dest)
        for name, (src, expected) in files.items():
            dest = stage / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dest)
            if digest(dest) != expected:
                raise ValueError(f'SDK checksum mismatch: {name}')
            dest.chmod(0o755 if name in ('bin/bolt', 'bin/bolt-watch.py', 'bin/llvm/bin/ld64.lld', 'examples/FunkeEditorBench/bin/macos-x64/BoltEditorSim',
                                              'examples/FunkeEditorBench/bin/macos-arm64/BoltEditorSim', 'setup.sh', 'uninstall.sh') else 0o644)
        state = dict(old, files={name: checksum for name, (_, checksum) in files.items()})
        record(stage, state)
        had_old = prefix.exists()
        if had_old:
            prefix.rename(backup)
        try:
            stage.rename(prefix)
            committed = True
        except OSError:
            if had_old:
                backup.rename(prefix)
            raise
        return state
    finally:
        if committed or not backup.exists():
            shutil.rmtree(work)
        else:
            print(f'The previous installation is preserved in {backup}', file=sys.stderr)


def configure_path(prefix, state):
    home = Path.home()
    login = [home / n for n in ('.bash_profile', '.bash_login') if (home / n).exists()]
    zsh = Path(os.environ.get('ZDOTDIR') or home).expanduser()
    paths = [home / '.profile', home / '.bashrc', *login[:1], zsh / '.zprofile', zsh / '.zshrc']
    quoted = shlex.quote(str(prefix / 'bin'))
    token = hashlib.sha256(str(prefix).encode()).hexdigest()[:12]
    begin = f'# >>> Bolt SDK {token} >>>\n'
    block = begin + f'case ":${{PATH-}}:" in\n  *:{quoted}:*) ;;\n  *) export PATH={quoted}:"${{PATH-}}" ;;\nesac\n' + f'# <<< Bolt SDK {token} <<<\n'
    for path in dict.fromkeys(p.resolve() for p in paths):
        old = path.read_bytes() if path.exists() else b''
        key = str(path)
        previous = state['profiles'].get(key)
        if previous and previous['block'].encode() in old:
            continue
        if begin.encode() in old:
            print(f'Preserved edited PATH entry in {path}', file=sys.stderr)
            continue
        addition = ('' if not old or old.endswith(b'\n') else '\n') + block
        state['profiles'][key] = {'block': addition, 'created': not path.exists()}
        record(prefix, state)
        save(path, old + addition.encode())


def remove_path(state):
    for key, item in state['profiles'].items():
        path = Path(key)
        if path.is_symlink() or not path.is_file():
            continue
        old = path.read_bytes()
        block = item['block'].encode()
        if old.count(block) == 1:
            before, _, after = old.partition(block)
            # If the original last line had no newline, keep it separate from later user edits.
            separator = b'\n' if block.startswith(b'\n') and before and after and not after.startswith(b'\n') else b''
            new = before + separator + after
            if item['created'] and not new:
                path.unlink()
            else:
                save(path, new)
        else:
            print(f'Preserved edited PATH entry in {path}', file=sys.stderr)


def extensions(code):
    result = subprocess.check_output([code, '--list-extensions', '--show-versions'], text=True)
    for line in result.splitlines():
        name, _, version = line.strip().partition('@')
        if name.lower() == EXTENSION:
            return version
    return None


def vscode(prefix, state, installing):
    code = shutil.which('code')
    vsix = prefix / 'Bolt-VSCode.vsix'
    managed = state.get('extension')
    if not code or (installing and not vsix.is_file()):
        if installing:
            print(f'VS Code extension available for manual installation: code --install-extension {shlex.quote(str(vsix))}' if vsix.is_file() else 'No VSIX in this SDK; install it separately.')
        elif managed:
            raise ValueError(f'VS Code CLI unavailable. Remove {EXTENSION} manually or use --skip-vscode.')
        return
    if not installing and not managed:
        return
    current = extensions(code)
    if installing:
        subprocess.run([code, '--install-extension', str(vsix), '--force'], check=True)
        version = extensions(code)
        if not version:
            raise ValueError('VS Code did not confirm the installed Bolt extension')
        state['extension'] = version if managed or current is None else None
        record(prefix, state)
    elif current == managed:
        subprocess.run([code, '--uninstall-extension', EXTENSION], check=True)
    elif current:
        print(f'Preserved separately updated Bolt extension {current}.')


def uninstall(prefix, state, skip):
    if not skip:
        vscode(prefix, state, False)
    remove_path(state)
    parents = set()
    for name, expected in state['files'].items():
        p = local(prefix, name)
        parents.update(d for d in p.parents if d == prefix or prefix in d.parents)
        if p.is_file() and digest(p) == expected:
            p.unlink()
        elif p.exists():
            print(f'Preserved modified file: {p}')
    (prefix / MARKER).unlink()
    for p in sorted(parents, key=lambda x: len(x.parts), reverse=True):
        try:
            p.rmdir()
        except OSError:
            pass
    print('Bolt removed. Open a new terminal.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('operation', choices=('install', 'uninstall'))
    p.add_argument('--prefix', type=Path)
    p.add_argument('--source', type=Path, default=HERE)
    p.add_argument('--skip-vscode', action='store_true')
    p.add_argument('--no-path', action='store_true', help='Install without editing Bash/Zsh start files')
    a = p.parse_args()
    installing = a.operation == 'install'
    migrated_from = None
    if a.prefix:
        prefix = a.prefix
    elif not installing and (HERE / MARKER).is_file():
        prefix = HERE
    elif sys.platform == 'darwin':
        prefix, migrated_from = macos_default_prefix(Path.home())
    else:
        prefix = Path.home() / '.local/share/bolt'
    prefix = prefix.expanduser().absolute()
    # macOS exposes its temporary directories through these system aliases.
    if sys.platform == 'darwin':
        for alias in (Path('/tmp'), Path('/var')):
            canonical = Path('/private') / alias.name
            if alias in prefix.parents and alias.is_symlink() and Path(os.readlink(alias)) in (canonical, canonical.relative_to('/')):
                prefix = canonical / prefix.relative_to(alias)
                break
    source = a.source.expanduser().resolve()
    home = Path.home().resolve()
    if prefix.resolve() != prefix or prefix == home or prefix in home.parents or any(c in str(prefix) for c in ':\r\n'):
        raise ValueError(f'Choose a separate, unambiguous install directory: {prefix}')
    if installing and (source == prefix or source in prefix.parents or prefix in source.parents):
        raise ValueError('Run setup from the extracted SDK with a separate install directory.')
    if installing:
        prefix.parent.mkdir(parents=True, exist_ok=True)
    elif not (prefix / MARKER).is_file():
        if not prefix.exists():
            print('Bolt SDK is already removed.')
            return
        raise ValueError(f'No portable Bolt SDK registered in {prefix}')
    lock = os.open(prefix.parent, os.O_RDONLY)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if prefix.exists() and any(prefix.iterdir()):
            if not (prefix / MARKER).is_file():
                raise ValueError(f'Unmanaged directory; choose --prefix or move the existing installation: {prefix}')
            state = load(prefix)
            if migrated_from and state.get('default_source_prefix') != migrated_from:
                raise ValueError(f'A separate SDK already uses {prefix}. Choose an explicit --prefix.')
        else:
            state = dict(product=PRODUCT, prefix=str(prefix), files={}, profiles={}, extension=None)
        if installing:
            if migrated_from and not state.get('default_source_prefix'):
                state['default_source_prefix'] = migrated_from
                print(f'Found the source installation at {migrated_from}. Installing the prebuilt SDK in {prefix}.')
            state = install_files(prefix, source, state)
            if not a.no_path:
                configure_path(prefix, state)
            print(f'Installed SDK: {prefix}')
            if not a.skip_vscode:
                vscode(prefix, state, True)
            print('Open a new Bash/Zsh terminal and run: bolt version')
            print(f'For this shell: export PATH={shlex.quote(str(prefix / "bin"))}:"$PATH"')
        else:
            uninstall(prefix, state, a.skip_vscode)
    finally:
        os.close(lock)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        sys.exit(f'Bolt setup: {e}')
