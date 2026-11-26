#!/usr/bin/env python3
"""Regression checks for Bolt's source-file icon assets and installer integration."""
from __future__ import annotations
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets'


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_installer():
    spec = importlib.util.spec_from_file_location('bolt_install_posix', ROOT / 'tools/install-posix.py')
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_assets() -> int:
    count = 0
    def ok(value, message):
        nonlocal count
        assert value, message
        count += 1
        print('PASS ' + message)

    file_master = Image.open(ASSETS / 'fileIcon.png').convert('RGBA')
    product_master = Image.open(ASSETS / 'icon.png').convert('RGBA')
    ok(file_master.size == (1254, 1254), 'fileIcon.png is the full-resolution source-file artwork')
    ok(product_master.size == (1254, 1254), 'icon.png is the full-resolution standalone Bolt artwork')
    ok(sha(ASSETS / 'fileIcon.png') != sha(ASSETS / 'icon.png'), 'source-file and standalone Bolt masters are distinct')
    expected16 = file_master.resize((16, 16), Image.Resampling.LANCZOS)
    actual16 = Image.open(ASSETS / 'bolt-file-16.png').convert('RGBA')
    ok(expected16.tobytes() == actual16.tobytes(), 'bolt-file-16.png is generated from fileIcon.png')
    expected32 = file_master.resize((32, 32), Image.Resampling.LANCZOS)
    actual32 = Image.open(ASSETS / 'bolt-file-32.png').convert('RGBA')
    ok(expected32.tobytes() == actual32.tobytes(), 'bolt-file-32.png is generated from fileIcon.png')
    ok(sha(ROOT / 'editors/intellij/src/main/resources/icons/bolt.png') == sha(ASSETS / 'bolt-file-16.png'), 'IntelliJ .bolt file type uses the source-file icon')
    ok((ASSETS / 'bolt-file.ico').is_file() and (ASSETS / 'bolt-file.icns').is_file(), 'Windows and macOS source-file icon formats exist')
    ok(Image.open(ASSETS / 'bolt.png').convert('RGBA').tobytes() == product_master.tobytes(), 'standalone bolt.png comes from icon.png, not fileIcon.png')

    installer = load_installer()
    with tempfile.TemporaryDirectory(prefix='bolt-macos-icon-test-') as td:
        stage = Path(td)
        bundle = installer.create_macos_file_icon_bundle(stage)
        info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
        doc = info['CFBundleDocumentTypes'][0]
        uti = info['UTExportedTypeDeclarations'][0]
        ok(doc['CFBundleTypeExtensions'] == ['bolt'] and doc['CFBundleTypeIconFile'] == 'bolt-file.icns', 'macOS bundle maps .bolt to bolt-file.icns')
        ok(doc['CFBundleTypeRole'] == 'None' and doc['LSHandlerRank'] == 'None', 'macOS icon metadata does not claim the default opener role')
        ok(uti['UTTypeIdentifier'] == 'dev.bolt.source' and uti['UTTypeTagSpecification']['public.filename-extension'] == ['bolt'], 'macOS dev.bolt.source UTI owns the .bolt extension metadata')

    windows = (ROOT / 'tools/install.ps1').read_text(encoding='utf-8')
    ok("assets\\bolt-file.ico" in windows and "Software\\Classes\\.bolt" in windows and '\\DefaultIcon' in windows, 'Windows installer registers bolt-file.ico for .bolt')
    posix = (ROOT / 'tools/install-posix.py').read_text(encoding='utf-8')
    ok("text/x-bolt" in posix and 'register_linux_file_icons' in posix, 'Linux installer registers a .bolt MIME icon')
    ok('register_macos_file_icons' in posix and 'lsregister' in posix, 'macOS installer registers file-icon metadata with LaunchServices')
    return count


def check_linux_install_roundtrip() -> int:
    if not sys.platform.startswith('linux'):
        print('SKIP Linux installer roundtrip (not Linux)')
        return 0
    with tempfile.TemporaryDirectory(prefix='bolt-icon-install-test-') as td:
        root = Path(td)
        home = root / 'home'
        home.mkdir()
        xdg = root / 'xdg'
        fakebin = root / 'fakebin'
        fakebin.mkdir()
        for name in ('bolt', 'libBoltCore.a', 'libBoltRuntime.so', 'BoltStart.o'):
            p = fakebin / name
            p.write_bytes(b'#!/bin/sh\nexit 0\n' if name == 'bolt' else b'fake')
            if name == 'bolt': p.chmod(0o755)
        prefix = home / 'bolt-install'
        env = os.environ.copy()
        env['HOME'] = str(home)
        env['XDG_DATA_HOME'] = str(xdg)
        subprocess.run([
            sys.executable, str(ROOT / 'tools/install-posix.py'), 'install',
            '--prefix', str(prefix), '--bin-dir', str(fakebin), '--no-path'
        ], check=True, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        mime = xdg / 'mime/packages/dev.bolt.source.xml'
        i16 = xdg / 'icons/hicolor/16x16/mimetypes/text-x-bolt.png'
        i32 = xdg / 'icons/hicolor/32x32/mimetypes/text-x-bolt.png'
        i256 = xdg / 'icons/hicolor/256x256/mimetypes/text-x-bolt.png'
        assert mime.is_file() and i16.is_file() and i32.is_file() and i256.is_file()
        assert sha(i16) == sha(ASSETS / 'bolt-file-16.png')
        assert sha(i32) == sha(ASSETS / 'bolt-file-32.png')
        assert sha(i256) == sha(ASSETS / 'bolt-file.png')
        marker = json.loads((prefix / '.bolt-install.json').read_text(encoding='utf-8'))
        assert marker['file_icons']['platform'] == 'linux'
        globs = xdg / 'mime/globs2'
        if globs.is_file():
            assert 'text/x-bolt:*.bolt' in globs.read_text(encoding='utf-8')
            print('PASS Linux MIME cache maps *.bolt to text/x-bolt')
        print('PASS Linux install.sh path installs MIME metadata and all configured file-icon sizes')
        subprocess.run([
            sys.executable, str(prefix / 'uninstall.py'), 'uninstall', '--prefix', str(prefix)
        ], check=True, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert not mime.exists() and not i16.exists() and not i32.exists() and not i256.exists()
        print('PASS Linux managed uninstall removes only its owned .bolt file-icon registration')
        return 3 if globs.is_file() else 2


def main() -> int:
    count = check_assets() + check_linux_install_roundtrip()
    print(f'{count} file-icon checks passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
