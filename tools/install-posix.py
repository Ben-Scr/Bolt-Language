#!/usr/bin/env python3
"""Per-user POSIX installer. Never runs sudo, downloads code, or edits system profiles."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile

PRODUCT = 'bolt-user-install-v1'
MARKER = '.bolt-install.json'
DEFAULT = Path.home() / '.local/share/bolt'
SOURCE = Path(__file__).resolve().parents[1]
MIME_TYPE = 'text/x-bolt'
MIME_ICON = 'text-x-bolt'
MAC_UTI = 'dev.bolt.source'

def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def safe_relative(value: str) -> Path:
    p = Path(value)
    if not value or p.is_absolute() or '..' in p.parts or p == Path('.'):
        raise ValueError('Invalid installation manifest path: ' + value)
    return p

def validate_prefix(path: Path, installing: bool) -> Path:
    path = path.expanduser().absolute()
    # macOS exposes its temporary directories through these system aliases.
    if sys.platform == 'darwin':
        for alias in (Path('/tmp'), Path('/var')):
            canonical = Path('/private') / alias.name
            if alias in path.parents and alias.is_symlink() and Path(os.readlink(alias)) in (canonical, canonical.relative_to('/')):
                path = canonical / path.relative_to(alias)
                break
    # Refuse all other symlink components, root/home and source-tree overlap.
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError('Installation path must not contain symbolic links: ' + str(path))
    path = path.resolve()
    if path == Path('/') or path == Path.home().resolve() or (installing and (path == SOURCE or path in SOURCE.parents or SOURCE in path.parents)):
        raise ValueError('Use a separate dedicated installation folder, not home/root/the source tree.')
    if any(c in str(path) for c in '\n\r\0:'):
        raise ValueError('Installation path cannot contain a colon or line break.')
    return path

def load_manifest(prefix: Path) -> dict:
    p = prefix / MARKER
    if p.is_symlink() or not p.is_file():
        raise ValueError('Not a managed Bolt installation: ' + str(prefix))
    data = json.loads(p.read_text(encoding='utf-8'))
    if data.get('product') != PRODUCT or data.get('prefix') != str(prefix):
        raise ValueError('Installation marker does not match this folder.')
    for name in data.get('files', {}):
        rel = safe_relative(name)
        if any(p.is_symlink() for p in (prefix / rel, *(prefix / rel).parents)):
            raise ValueError('Refusing a symbolic link in the installed payload: ' + name)
    return data

def write_atomic(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    old_mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o644
    with tempfile.NamedTemporaryFile('w', dir=path.parent, delete=False, encoding='utf-8') as f:
        temporary = Path(f.name)
        f.write(content)
    os.chmod(temporary, old_mode)
    temporary.replace(path)

def profile_paths() -> list[Path]:
    home = Path.home()
    paths = [home / '.profile', home / '.bashrc']
    for name in ('.bash_profile', '.bash_login'):
        if (home / name).exists():
            paths.append(home / name)
            break
    zhome = Path(os.environ.get('ZDOTDIR', str(home))).expanduser()
    paths += [zhome / '.zprofile', zhome / '.zshrc']
    return list(dict.fromkeys(p.absolute() for p in paths))

def configure_path(prefix: Path, previous: dict) -> dict[str, str]:
    key = hashlib.sha256(str(prefix).encode()).hexdigest()[:16]
    quoted = shlex.quote(str(prefix / 'bin'))
    block = ('# >>> Bolt PATH ' + key + ' >>>\n'
             + 'case ":${PATH-}:" in\n'
             + '  *:' + quoted + ':*) ;;\n'
             + '  *) export PATH=' + quoted + ':"${PATH-}" ;;\n'
             + 'esac\n# <<< Bolt PATH ' + key + ' <<<\n')
    profiles = dict(previous.get('profiles', {}))
    for path in profile_paths():
        # Preserve intentional dotfile symlinks by editing their target atomically.
        target = path.resolve() if path.is_symlink() else path
        if target.exists() and not target.is_file():
            raise ValueError('Not a regular shell profile: ' + str(target))
        old = target.read_text(encoding='utf-8') if target.exists() else ''
        old_block = profiles.get(str(target), '')
        new = old.replace(old_block, '') if old_block else old
        # Markers prevent duplicate additions across reinstalls.
        new = new.replace(block, '')
        if new and not new.endswith('\n'):
            new += '\n'
        new += block
        if new != old:
            if target.exists():
                backup = target.with_name(target.name + '.bolt-backup-' + key)
                if not backup.exists():
                    shutil.copy2(target, backup)
            write_atomic(target, new)
        profiles[str(target)] = block
    return profiles

def _external_file_record(path: Path, content: bytes, previous: dict | None) -> dict:
    """Install one per-user desktop integration file without clobbering foreign data."""
    if path.is_symlink():
        raise ValueError('Refusing symlinked desktop integration file: ' + str(path))
    if previous:
        if previous.get('path') != str(path):
            raise ValueError('Desktop integration path changed unexpectedly: ' + str(path))
        expected = previous.get('sha256')
        if path.exists() and expected and digest(path) != expected:
            print('Preserved modified Bolt desktop integration file: ' + str(path))
            return dict(previous, preserved=True)
    elif path.exists():
        raise ValueError('Refusing to overwrite an unowned desktop integration file: ' + str(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.bolt-tmp')
    temporary.write_bytes(content)
    os.chmod(temporary, 0o644)
    temporary.replace(path)
    return {'path': str(path), 'sha256': digest(path)}

def _ensure_linux_hicolor_theme(data_home: Path) -> None:
    """Make the per-user hicolor theme cacheable by GTK file managers."""
    theme_root = data_home / 'icons/hicolor'
    theme_root.mkdir(parents=True, exist_ok=True)
    index = theme_root / 'index.theme'
    if index.exists():
        if index.is_symlink() or not index.is_file():
            raise ValueError('Invalid hicolor theme index: ' + str(index))
        return

    system_index = Path('/usr/share/icons/hicolor/index.theme')
    if system_index.is_file():
        shutil.copy2(system_index, index)
        os.chmod(index, 0o644)
        return

    # Minimal freedesktop-compatible fallback for systems without hicolor-icon-theme.
    index.write_text(
        '[Icon Theme]\n'
        'Name=Hicolor\n'
        'Comment=Fallback icon theme\n'
        'Hidden=true\n'
        'Directories=16x16/mimetypes,32x32/mimetypes,256x256/mimetypes,scalable/mimetypes\n'
        '\n'
        '[16x16/mimetypes]\nSize=16\nContext=MimeTypes\nType=Fixed\n'
        '\n'
        '[32x32/mimetypes]\nSize=32\nContext=MimeTypes\nType=Fixed\n'
        '\n'
        '[256x256/mimetypes]\nSize=256\nContext=MimeTypes\nType=Fixed\n'
        '\n'
        '[scalable/mimetypes]\nSize=128\nMinSize=16\nMaxSize=512\nContext=MimeTypes\nType=Scalable\n',
        encoding='utf-8')
    os.chmod(index, 0o644)

def _refresh_linux_file_icons(data_home: Path) -> None:
    _ensure_linux_hicolor_theme(data_home)
    update_mime = shutil.which('update-mime-database')
    if update_mime:
        # The database updater also requires its packages directory when empty.
        (data_home / 'mime/packages').mkdir(parents=True, exist_ok=True)
        result = subprocess.run([update_mime, str(data_home / 'mime')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode != 0:
            print('Bolt installer: warning: update-mime-database could not refresh the user MIME cache.', file=sys.stderr)
    else:
        print('Bolt installer: warning: update-mime-database is unavailable; the .bolt icon may appear after the desktop refreshes its MIME cache.', file=sys.stderr)
    gtk_cache = shutil.which('gtk-update-icon-cache')
    if gtk_cache:
        result = subprocess.run([gtk_cache, '-f', str(data_home / 'icons/hicolor')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode != 0:
            print('Bolt installer: warning: GTK could not refresh the user icon cache.', file=sys.stderr)

def _linux_icon_targets(data_home: Path) -> dict[str, Path]:
    return {
        'mime': data_home / 'mime/packages/dev.bolt.source.xml',
        'icon16': data_home / 'icons/hicolor/16x16/mimetypes/text-x-bolt.png',
        'icon32': data_home / 'icons/hicolor/32x32/mimetypes/text-x-bolt.png',
        'icon256': data_home / 'icons/hicolor/256x256/mimetypes/text-x-bolt.png',
        'icon_svg': data_home / 'icons/hicolor/scalable/mimetypes/text-x-bolt.svg',
    }

def register_linux_file_icons(prefix: Path, previous: dict | None) -> dict:
    data_home = Path(os.environ.get('XDG_DATA_HOME', str(Path.home() / '.local/share'))).expanduser().absolute()
    previous = previous if previous and previous.get('platform') == 'linux' else None
    if previous and Path(previous.get('data_home', '')).absolute() != data_home:
        print('Bolt installer: XDG_DATA_HOME changed; the previous Bolt file-icon registration was left untouched.', file=sys.stderr)
        previous = None
    targets = _linux_icon_targets(data_home)
    payloads = {
        'mime': (prefix / 'share/bolt/icons/dev.bolt.source.xml').read_bytes(),
        'icon16': (prefix / 'share/bolt/icons/bolt-file-16.png').read_bytes(),
        'icon32': (prefix / 'share/bolt/icons/bolt-file-32.png').read_bytes(),
        'icon256': (prefix / 'share/bolt/icons/bolt-file.png').read_bytes(),
        'icon_svg': (prefix / 'share/bolt/icons/bolt-file.svg').read_bytes(),
    }
    old_files = previous.get('files', {}) if previous else {}
    # Preflight every target before mutating any of them.
    for key, path in targets.items():
        old = old_files.get(key)
        if path.is_symlink():
            raise ValueError('Refusing symlinked desktop integration file: ' + str(path))
        if path.exists() and not old:
            raise ValueError('Refusing to overwrite an unowned desktop integration file: ' + str(path))
        if old and old.get('path') == str(path) and path.exists() and old.get('sha256') and digest(path) != old['sha256']:
            print('Preserved modified Bolt desktop integration file: ' + str(path))
    records = {}
    for key, path in targets.items():
        old = old_files.get(key)
        if old and path.exists() and old.get('sha256') and digest(path) != old['sha256']:
            records[key] = dict(old, preserved=True)
            continue
        records[key] = _external_file_record(path, payloads[key], old)
    _refresh_linux_file_icons(data_home)
    print('Registered the Bolt file icon for *.bolt in the user MIME database.')
    return {'platform': 'linux', 'data_home': str(data_home), 'files': records}

def unregister_linux_file_icons(registration: dict) -> None:
    if not registration or registration.get('platform') != 'linux':
        return
    data_home = Path(registration.get('data_home', '')).expanduser().absolute()
    targets = _linux_icon_targets(data_home)
    records = registration.get('files', {})
    for key, path in targets.items():
        old = records.get(key)
        if not old or old.get('path') != str(path):
            continue
        if path.is_symlink():
            print('Preserved symlinked Bolt desktop integration file: ' + str(path))
            continue
        if path.is_file() and old.get('sha256') and digest(path) == old['sha256']:
            path.unlink()
        elif path.exists():
            print('Preserved modified Bolt desktop integration file: ' + str(path))
    # Remove only directories Bolt may have made and only when empty.
    for path in sorted({p.parent for p in targets.values()}, key=lambda p: len(p.parts), reverse=True):
        try: path.rmdir()
        except OSError: pass
    _refresh_linux_file_icons(data_home)

def create_macos_file_icon_bundle(stage: Path) -> Path:
    bundle = stage / 'share/bolt/BoltFileIcon.app'
    contents = bundle / 'Contents'
    macos = contents / 'MacOS'
    resources = contents / 'Resources'
    macos.mkdir(parents=True, exist_ok=True)
    resources.mkdir(parents=True, exist_ok=True)
    executable = macos / 'BoltFileIcon'
    executable.write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
    os.chmod(executable, 0o755)
    shutil.copy2(SOURCE / 'assets/bolt-file.icns', resources / 'bolt-file.icns')
    info = {
        'CFBundleDevelopmentRegion': 'en',
        'CFBundleExecutable': 'BoltFileIcon',
        'CFBundleIdentifier': 'dev.bolt.fileicon',
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': 'Bolt File Icon',
        'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': '1.0',
        'CFBundleVersion': '1',
        'CFBundleDocumentTypes': [{
            'CFBundleTypeName': 'Bolt Source File',
            'CFBundleTypeRole': 'None',
            'LSHandlerRank': 'None',
            'LSItemContentTypes': [MAC_UTI],
            'CFBundleTypeExtensions': ['bolt'],
            'CFBundleTypeIconFile': 'bolt-file.icns',
        }],
        'UTExportedTypeDeclarations': [{
            'UTTypeIdentifier': MAC_UTI,
            'UTTypeDescription': 'Bolt Source File',
            'UTTypeConformsTo': ['public.source-code', 'public.text'],
            'UTTypeIconFile': 'bolt-file.icns',
            'UTTypeTagSpecification': {
                'public.filename-extension': ['bolt'],
                'public.mime-type': MIME_TYPE,
            },
        }],
    }
    with (contents / 'Info.plist').open('wb') as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_XML, sort_keys=True)
    return bundle

def _lsregister() -> Path | None:
    candidates = [
        Path('/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'),
        Path('/System/Library/Frameworks/ApplicationServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    found = shutil.which('lsregister')
    return Path(found) if found else None

def register_macos_file_icons(prefix: Path, previous: dict | None) -> dict | None:
    command = _lsregister()
    bundle = prefix / 'share/bolt/BoltFileIcon.app'
    if command is None:
        print('Skipped the Bolt file icon: macOS LaunchServices lsregister was not found.', file=sys.stderr)
        return previous
    try:
        subprocess.run([str(command), '-f', str(bundle)], check=True)
    except (OSError, subprocess.CalledProcessError) as error:
        print('Could not register the Bolt file icon: ' + str(error), file=sys.stderr)
        return previous
    print('Registered the Bolt file icon for *.bolt with macOS LaunchServices.')
    return {'platform': 'darwin', 'bundle': str(bundle)}

def unregister_macos_file_icons(registration: dict) -> None:
    if not registration or registration.get('platform') != 'darwin':
        return
    command = _lsregister()
    bundle = Path(registration.get('bundle', ''))
    if command and bundle.is_dir():
        try:
            subprocess.run([str(command), '-u', str(bundle)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except (OSError, subprocess.CalledProcessError) as error:
            print('Could not unregister the Bolt file icon: ' + str(error), file=sys.stderr)

def register_posix_file_icons(prefix: Path, previous: dict | None) -> dict | None:
    if sys.platform == 'darwin':
        return register_macos_file_icons(prefix, previous)
    if sys.platform.startswith('linux'):
        return register_linux_file_icons(prefix, previous)
    return previous

def unregister_posix_file_icons(registration: dict | None) -> None:
    if not registration:
        return
    if registration.get('platform') == 'darwin':
        unregister_macos_file_icons(registration)
    elif registration.get('platform') == 'linux':
        unregister_linux_file_icons(registration)

def install(args: argparse.Namespace, prefix: Path) -> None:
    previous: dict = {}
    if prefix.exists():
        if not prefix.is_dir():
            raise ValueError('Installation path is not a directory.')
        if any(prefix.iterdir()):
            previous = load_manifest(prefix)
    if args.bin_dir:
        bindir = args.bin_dir.expanduser().resolve()
    else:
        if not shutil.which('cmake'):
            raise ValueError('CMake 3.21+ is required. Install it or use --bin-dir with an existing build.')
        build = (args.build_dir or SOURCE / 'build/install').expanduser().resolve()
        subprocess.run(['cmake', '-S', str(SOURCE), '-B', str(build),
                        '-DCMAKE_BUILD_TYPE=Release', '-DBOLT_BUILD_TESTS=OFF',
                        '-DBOLT_BUILD_EMBEDDING=OFF'], check=True)
        subprocess.run(['cmake', '--build', str(build), '--config', 'Release',
                        '--parallel', str(args.jobs)], check=True)
        bindir = build / 'bin'
        if (bindir / 'Release/bolt').is_file():
            bindir /= 'Release'
    compiler, library = bindir / 'bolt', bindir / 'libBoltCore.a'
    native_names = (['libBoltRuntime.dylib', 'libBoltCompiler.dylib'] if sys.platform == 'darwin'
                    else ['libBoltRuntime.so', 'libBoltCompiler.so'])
    if sys.platform.startswith('linux'):
        native_names.append('BoltStart.o')
    payload = [compiler, library] + [bindir / name for name in native_names]
    if sys.platform.startswith("linux") and (bindir / "BoltAotRuntime.o").is_file():
        payload.append(bindir / "BoltAotRuntime.o")
    missing = [p.name for p in payload if not p.is_file()]
    if missing:
        raise ValueError('--bin-dir is missing native build artifacts: ' + ', '.join(missing))
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.bolt-stage-', dir=prefix.parent) as temporary:
        stage = Path(temporary)
        (stage / 'bin').mkdir()
        shutil.copy2(compiler, stage / 'bin/bolt')
        os.chmod(stage / 'bin/bolt', 0o755)
        shutil.copy2(SOURCE / 'tools/bolt-watch.py', stage / 'bin/bolt-watch.py')
        os.chmod(stage / 'bin/bolt-watch.py', 0o755)
        for artifact in payload[1:]:
            shutil.copy2(artifact, stage / 'bin' / artifact.name)
        shutil.copytree(SOURCE / 'include/bolt', stage / 'bin/include/bolt')
        (stage / 'share/bolt').mkdir(parents=True)
        for name in ('README.md', 'LICENSE'):
            shutil.copy2(SOURCE / name, stage / 'share/bolt' / name)
        shutil.copytree(SOURCE / 'docs/licenses', stage / 'share/bolt/licenses')
        shutil.copytree(SOURCE / 'stdlib', stage / 'share/bolt/stdlib')
        icons = stage / 'share/bolt/icons'
        icons.mkdir(parents=True)
        for name in ('bolt-file.png', 'bolt-file-16.png', 'bolt-file-32.png', 'bolt-file.svg', 'bolt-file.icns', 'dev.bolt.source.xml'):
            shutil.copy2(SOURCE / 'assets' / name, icons / name)
        if sys.platform == 'darwin':
            create_macos_file_icon_bundle(stage)
        shutil.copy2(SOURCE / 'tools/install-posix.py', stage / 'uninstall.py')
        (stage / 'uninstall.sh').write_text('#!/bin/sh\nexec python3 "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/uninstall.py" uninstall --prefix "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)" "$@"\n')
        os.chmod(stage / 'uninstall.sh', 0o755)
        files = {str(p.relative_to(stage)): digest(p) for p in stage.rglob('*') if p.is_file()}
        # Reject collisions rather than overwriting a user's unrelated file.
        for name in files:
            dest = prefix / name
            if any(p.is_symlink() for p in (dest, *dest.parents)):
                raise ValueError('Refusing symlink destination: ' + str(dest))
            if dest.exists() and name not in previous.get('files', {}):
                raise ValueError('Refusing to overwrite an unowned file: ' + str(dest))
            if dest.exists() and digest(dest) != previous['files'][name]:
                raise ValueError('Installed file was modified; preserve it before updating: ' + str(dest))
        prefix.mkdir(parents=True, exist_ok=True)
        # Publish each staged inode atomically. An editor can keep the old
        # compiler library mapped throughout an SDK update or uninstall.
        for name in files:
            dest = prefix / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            os.replace(stage / name, dest)
        for name, checksum in previous.get('files', {}).items():
            if name not in files:
                p = prefix / safe_relative(name)
                if p.is_file() and digest(p) == checksum:
                    p.unlink()
        header = (SOURCE / 'include/bolt/Version.hpp').read_text(encoding='utf-8')
        release_version = next(line.split(chr(34))[1] for line in header.splitlines()
                               if line.startswith('#define BOLT_VERSION_STRING '))
        manifest = {'product': PRODUCT, 'version': release_version, 'prefix': str(prefix),
                    'files': files, 'profiles': previous.get('profiles', {}),
                    'file_icons': previous.get('file_icons')}
        # Save ownership before touching shell profiles so uninstall stays available.
        write_atomic(prefix / MARKER, json.dumps(manifest, indent=2) + '\n')
        if not args.no_path:
            manifest['profiles'] = configure_path(prefix, previous)
            write_atomic(prefix / MARKER, json.dumps(manifest, indent=2) + '\n')
        manifest['file_icons'] = register_posix_file_icons(prefix, previous.get('file_icons'))
        write_atomic(prefix / MARKER, json.dumps(manifest, indent=2) + '\n')
    print('Installed Bolt to ' + str(prefix))
    print('Open a NEW terminal, then run: bolt help')
    print('For this shell now: export PATH=' + shlex.quote(str(prefix / 'bin')) + ':"$PATH"')

def uninstall(prefix: Path) -> None:
    manifest = load_manifest(prefix)
    unregister_posix_file_icons(manifest.get('file_icons'))
    for filename, block in manifest.get('profiles', {}).items():
        p = Path(filename)
        if p.is_symlink():
            p = p.resolve()
        if p.is_file():
            old = p.read_text(encoding='utf-8')
            if block in old:
                write_atomic(p, old.replace(block, ''))
            else:
                print('Preserved modified/missing PATH block in ' + str(p))
    preserved = []
    for filename, checksum in manifest['files'].items():
        p = prefix / safe_relative(filename)
        if p.is_file() and digest(p) == checksum:
            p.unlink()
        elif p.exists():
            preserved.append(str(p))
    (prefix / MARKER).unlink()
    for p in sorted(prefix.rglob('*'), key=lambda p: len(p.parts), reverse=True):
        if p.is_dir() and not p.is_symlink():
            try: p.rmdir()
            except OSError: pass
    try: prefix.rmdir()
    except OSError: pass
    print('Uninstalled Bolt. Open a new terminal to refresh PATH.')
    for p in preserved:
        print('Preserved modified file: ' + p)

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('install', 'uninstall'))
    parser.add_argument('--prefix', type=Path, default=DEFAULT)
    parser.add_argument('--bin-dir', type=Path, help='Use an already built CLI/archive instead of building')
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--no-path', action='store_true', help='Do not edit shell startup files')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    try:
        if args.jobs < 1: raise ValueError('--jobs must be positive')
        prefix = validate_prefix(args.prefix, args.operation == 'install')
        if args.operation == 'install': install(args, prefix)
        else: uninstall(prefix)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print('Bolt installer: ' + str(error), file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
