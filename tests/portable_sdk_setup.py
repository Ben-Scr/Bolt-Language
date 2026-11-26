#!/usr/bin/env python3
"""Exercise a packaged Bolt SDK installer in an isolated user directory."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

HELPERS = Path(__file__).resolve().parents[1] / 'tools/sdk-setup'
EXT = 'ben-schneider.bolt-language'
SHIM = """#!/bin/sh
set -eu
case "$1" in
    --list-extensions) test ! -s "$HOME/extension" || cat "$HOME/extension";;
    --install-extension) test -f "$2"; test "$3" = "--force"; echo ben-schneider.bolt-language@1.1 > "$HOME/extension";;
    --uninstall-extension) test "$2" = ben-schneider.bolt-language; rm "$HOME/extension";;
    *) exit 2;;
esac
"""


def run(command, env, ok=True):
    result = subprocess.run(list(map(str, command)), cwd=Path(env['HOME']), env=env, text=True, capture_output=True)
    if (result.returncode == 0) != ok:
        raise AssertionError(f'{command} => {result.returncode}\n{result.stdout}{result.stderr}')
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sdk', type=Path)
    parser.add_argument('--pwsh', type=Path, help='Exercise the Windows installer file/VSIX logic on Linux without HKCU')
    args = parser.parse_args()
    windows = bool(args.pwsh)
    with tempfile.TemporaryDirectory(prefix='Bolt SDK setup ') as tmp:
        root = Path(tmp).resolve()
        with zipfile.ZipFile(args.sdk) as archive:
            src = root / archive.namelist()[0].split('/')[0]
            archive.extractall(root)
        scripts = ['setup.cmd', 'uninstall.cmd', 'sdk-setup.ps1'] if windows else ['setup.sh', 'uninstall.sh', 'sdk-setup.py']
        for f in scripts + ['SETUP.md']:
            shutil.copy2(HELPERS / f, src / f)
        if not (src / 'Bolt-VSCode.vsix').exists():
            # The CLI shim checks invocation/ownership independently of VS Code's own VSIX tests.
            (src / 'Bolt-VSCode.vsix').write_text('VSIX fixture')
        sums = src / 'SHA256SUMS'
        def manifest():
            sums.write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(src).as_posix()}\n' for p in sorted(src.rglob('*')) if p.is_file() and p != sums))
        manifest()
        user = root / 'user profile'
        user.mkdir()
        stub = root / 'shim'
        stub.mkdir()
        (stub / 'code').write_text(SHIM)
        (stub / 'code').chmod(0o755)
        env = dict(os.environ, HOME=str(user), ZDOTDIR=str(user), PATH=str(stub) + ':' + os.environ['PATH'])
        dest = user / "Development & Tool's SDK"
        profile = user / '.profile'
        bashrc = user / '.bashrc'
        bashrc.write_bytes(b'# existing')
        profile.write_bytes(b'export LOCAL_SETTING=on')  # Exact round trip incl. missing terminal newline.
        if windows:
            flags = lambda action: [str(args.pwsh.resolve()), '-NoProfile','-File',src/'sdk-setup.ps1','-Action',action,'-Prefix',dest,'-NoPath']
        else:
            flags = lambda action: ['sh', src / ('setup.sh' if action == 'Install' else 'uninstall.sh'), '--prefix', dest]

        run(flags('Install'), env)
        exe = dest / ('bin/bolt.exe' if windows else 'bin/bolt')
        assert exe.is_file() and (user / 'extension').read_text().strip() == EXT + '@1.1'
        if not windows:
            assert exe.stat().st_mode & 0o111
            assert run([exe, 'version'], env).strip() == run(['bash', '--noprofile', '--norc', '-c', '. "$HOME/.profile"; exec bolt version'], env).strip()
            run([exe, 'init', user/'hello project'], env)
            assert 'Hello from Bolt' in run([exe, 'run', user/'hello project', '--debug', '--backend', 'auto',
                                              '--no-debug-info', '--require-native'], env)
            from ctypes import CDLL
            assert CDLL(str(dest / 'bin' / ('libBoltCompiler.dylib' if sys.platform == 'darwin' else 'libBoltCompiler.so'))).bolt_compiler_host_abi() == 1
            assert profile.read_bytes().count(b'# >>> Bolt SDK') == 1
        foreign = dest / 'my notes.txt'
        foreign.write_text('keep')
        (src / 'README.md').write_text('updated documentation')
        manifest()
        run(flags('Install'), env)
        assert (dest / 'README.md').read_text() == 'updated documentation' and foreign.read_text() == 'keep'
        old = exe.read_bytes()
        (src / 'README.md').write_text('tampered README')
        assert 'checksum mismatch' in run(flags('Install'), env, False)
        assert exe.read_bytes() == old and (dest / 'README.md').read_text() == 'updated documentation'
        (src / 'README.md').write_text('updated documentation')
        (dest / 'README.md').write_text('mine')
        assert 'Modified SDK file' in run(flags('Install'), env, False)
        if not windows:
            with bashrc.open('ab') as f:
                f.write(b'export MY_EDIT=yes\n')
        if windows:
            run([args.pwsh.resolve(), '-NoProfile', '-File',dest/'sdk-setup.ps1','-Action','Uninstall'], env)
        else:
            run(['sh', dest/'uninstall.sh'], env)
        assert not exe.exists() and foreign.read_text() == 'keep' and (dest/'README.md').read_text()=='mine'
        assert not (user / 'extension').exists()
        assert profile.read_bytes() == b'export LOCAL_SETTING=on'
        if not windows:
            assert bashrc.read_bytes() == b'# existing\nexport MY_EDIT=yes\n'
        assert 'Unmanaged directory' in run(flags('Install'), env, False)
        shutil.rmtree(dest)
        (user / 'extension').write_text(EXT+'@0.9\n')
        run(flags('Install'), env)
        run(flags('Uninstall'), env)
        assert (user / 'extension').exists() and not dest.exists()
        run(flags('Uninstall'), env)
        print('PASS: spaces, VSIX install/upgrade/ownership, SDK upgrade, checksum failure rollback, modified/foreign files, installed uninstaller, repeat uninstall')
        if not windows:
            print('PASS: actual host bolt version through fresh shell PATH, native Debug compile/run, host ABI, exact profile restoration')


if __name__ == '__main__':
    main()
