#!/usr/bin/env python3
"""Check the macOS bundle selector and ownership rules without Apple hardware."""
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SDK_SETUP = runpy.run_path(str(ROOT / 'tools/sdk-setup/sdk-setup.py'))


def test_bundle(root):
    bundle = root / 'bundle'
    package = bundle / 'sdk/Bolt-0.1.0-F5-macOS-arm64.zip'
    package.parent.mkdir(parents=True)
    package.touch()
    archive = bundle / 'source/Bolt-0.1.0-F5-Quellcode.zip'
    archive.parent.mkdir()
    archive.touch()
    shell = bundle / 'install.sh'
    shutil.copy2(ROOT / 'tools/distribution/install.sh', shell)
    commands = root / 'commands'
    commands.mkdir()
    for name, content in {
        'uname': 'case "$1" in -s) echo Darwin;; -m) echo arm64;; *) exit 2;; esac',
        'sw_vers': 'test "$1" = -productVersion && printf "%s\\n" "$MAC_VERSION"',
    }.items():
        tool = commands / name
        tool.write_text('#!/bin/sh\n' + content + '\n')
        tool.chmod(0o755)

    def plan(version, *flags):
        env = dict(os.environ, PATH=f'{commands}:{os.environ["PATH"]}', MAC_VERSION=version)
        return subprocess.run(['sh', shell, '--plan', *flags], env=env, capture_output=True, text=True)

    before = plan('12.7.6')
    assert before.returncode == 0 and str(archive) in before.stdout
    assert 'requires macOS 13' in before.stderr and '12.7.6' in before.stderr
    for version in ('13.0', '14.6.1'):
        selected = plan(version)
        assert selected.returncode == 0 and str(package) in selected.stdout and not selected.stderr
    assert str(archive) in plan('13.0', '--from-source').stdout
    archive.unlink()
    missing = plan('12.7.6')
    assert missing.returncode != 0 and 'requires macOS 13' in missing.stderr and 'Extract the whole' in missing.stderr
    print('PASS macOS 12, 13, 14, explicit source and missing-source plans')


def test_ownership(home):
    primary = home / '.local/share/bolt'
    alternate = primary.with_name('bolt-sdk')
    legacy = primary / '.bolt-install.json'
    marker = alternate / SDK_SETUP['MARKER']
    select = SDK_SETUP['macos_default_prefix']

    def write(file, product, **extra):
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(json.dumps(dict(prefix=str(file.parent), files={'bin/bolt': 'sample'}, product=product, **extra)))

    assert select(home) == (primary, None)
    legacy.parent.mkdir(parents=True)
    legacy.write_text('unreadable JSON')
    assert select(home) == (primary, None)
    write(legacy, 'foreign')
    assert select(home) == (primary, None)
    write(legacy, 'bolt-user-install-v1')
    assert select(home) == (alternate, str(primary))
    legacy.rename(legacy.with_suffix('.other'))
    legacy.symlink_to(legacy.with_suffix('.other'))
    assert select(home) == (primary, None)
    legacy.unlink()
    legacy.with_suffix('.other').rename(legacy)
    write(marker, SDK_SETUP['PRODUCT'])
    try:
        select(home)
    except ValueError as error:
        assert 'explicit --prefix' in str(error)
    else:
        raise AssertionError('accepted an independently installed SDK')
    write(marker, SDK_SETUP['PRODUCT'], default_source_prefix=str(primary))
    assert select(home) == (alternate, str(primary))
    legacy.unlink()
    assert select(home) == (alternate, str(primary))
    write(primary / SDK_SETUP['MARKER'], SDK_SETUP['PRODUCT'])
    assert select(home) == (primary, None)
    print('PASS managed defaults, independent target, invalid manifest, symlink, later upgrade and primary precedence')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='Bolt macOS install ') as temp:
        work = Path(temp).resolve()
        test_bundle(work)
        test_ownership(work / 'user home')
