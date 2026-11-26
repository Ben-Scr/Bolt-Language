#!/usr/bin/env python3
"""Sandboxed per-user installation tests; never changes the tester's real HOME."""
import argparse
import ctypes
import sys
import json
import os
from pathlib import Path
import subprocess
import tempfile
import shutil

ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('--bolt',type=Path,required=True);p.add_argument('--cxx',default='c++');a=p.parse_args()
    count=0
    def passed(label):
        nonlocal count
        count+=1;print('PASS '+label)
    if os.name=='nt':print('SKIP POSIX installer on Windows');return
    with tempfile.TemporaryDirectory(prefix='bolt-installer-') as directory:
        tmp=Path(directory).resolve();home=tmp/'home';home.mkdir();prefix=tmp/'installed Bolt ü'
        env=os.environ.copy();env['HOME']=str(home);env.pop('ZDOTDIR',None)
        env['XDG_DATA_HOME'] = str(home / '.local/share')
        env['XDG_CONFIG_HOME'] = str(home / '.config')
        env['XDG_CACHE_HOME'] = str(home / '.cache')
        env['XDG_STATE_HOME'] = str(home / '.local/state')
        (home/'.profile').write_text('# preserve me\nexport OTHER_VALUE=123\n')
        (home/'.bash_profile').write_text('# login profile\n')
        def install(*flags):
            return subprocess.run(['sh',str(ROOT/'install.sh'),'--bin-dir',str(a.bolt.resolve().parent),'--prefix',str(prefix),*flags],env=env,capture_output=True,text=True,timeout=120)
        r=install();assert r.returncode==0,r.stdout+r.stderr
        assert (prefix/'bin/bolt').is_file() and (prefix/'bin/include/bolt/Diagnostics.hpp').is_file()
        passed('installation includes executable, archive and matching headers')
        assert os.access(prefix/'bin/bolt-watch.py', os.X_OK)
        assert (prefix/'bin/include/bolt/CompilerHost.h').is_file()
        host_name = 'libBoltCompiler.dylib' if sys.platform == 'darwin' else 'libBoltCompiler.so'
        installed_host = ctypes.CDLL(str(prefix / 'bin' / host_name))
        installed_host.bolt_compiler_host_abi.restype = ctypes.c_uint32
        assert installed_host.bolt_compiler_host_abi() == 1
        passed('installed host SDK header and runtime ABI are available')
        assert (home/'.profile').read_text().startswith('# preserve me\nexport OTHER_VALUE=123\n')
        passed('existing shell profile content preserved')
        marker=json.loads((prefix/'.bolt-install.json').read_text())
        assert len(marker['profiles'])==5
        for profile in marker['profiles']:
            assert Path(profile).read_text().count('# >>> Bolt PATH ')==1
        passed('Bash and Zsh startup profiles receive marked PATH entries')
        before={p:Path(p).read_text() for p in marker['profiles']}
        r=install();assert r.returncode==0,r.stdout+r.stderr
        assert before=={p:Path(p).read_text() for p in marker['profiles']}
        assert installed_host.bolt_compiler_host_abi() == 1
        passed('reinstall preserves the active compiler library and does not duplicate PATH blocks')
        r=subprocess.run(['bash','--noprofile','--norc','-c','. "$HOME/.profile"; bolt version'],env=env,cwd=tmp,capture_output=True,text=True,timeout=15)
        assert r.returncode==0 and 'Bolt 0.1.0' in r.stdout,r.stdout+r.stderr
        passed('bolt command resolves from an unrelated working directory')
        assert (home/'.profile').with_name('.profile.bolt-backup-'+__import__('hashlib').sha256(str(prefix).encode()).hexdigest()[:16]).is_file()
        passed('original profile backup created')
        project=tmp/'Hello';subprocess.run([str(prefix/'bin/bolt'),'new',str(project)],check=True,capture_output=True)
        r=subprocess.run([str(prefix/'bin/bolt'),'compile',str(project),'--cxx',a.cxx,'--no-pch'],capture_output=True,text=True,timeout=120,env=env)
        assert r.returncode==0,r.stdout+r.stderr
        exe=project/'bin/Debug/Hello';assert subprocess.check_output([str(exe)]).decode().strip()=='Hello from Bolt!'
        passed('installed compiler builds a native program using its installed runtime')
        extra=prefix/'my-notes.txt';extra.write_text('keep me')
        modified=prefix/'share/bolt/README.md';modified.write_text('user changed this')
        r=install();assert r.returncode!=0 and 'modified' in r.stderr
        passed('installer refuses to overwrite a modified installed file')
        r=subprocess.run(['sh',str(prefix/'uninstall.sh')],env=env,capture_output=True,text=True,timeout=30)
        assert r.returncode==0,r.stdout+r.stderr
        assert not (prefix/'bin/bolt').exists() and extra.read_text()=='keep me' and modified.read_text()=='user changed this'
        assert installed_host.bolt_compiler_host_abi() == 1
        passed('uninstall preserves the live compiler mapping and foreign or modified files')
        assert all('Bolt PATH' not in Path(f).read_text() for f in before)
        assert (home/'.profile').read_text()=='# preserve me\nexport OTHER_VALUE=123\n'
        passed('uninstall restores PATH blocks without deleting user settings')
        r=subprocess.run(['sh',str(ROOT/'uninstall.sh'),'--prefix',str(tmp)],env=env,capture_output=True,text=True)
        assert r.returncode!=0
        passed('uninstaller refuses an unowned directory')
        unmanaged=tmp/'unmanaged';unmanaged.mkdir();(unmanaged/'important').write_text('safe')
        r=subprocess.run(['sh',str(ROOT/'install.sh'),'--bin-dir',str(a.bolt.resolve().parent),'--prefix',str(unmanaged)],env=env,capture_output=True,text=True)
        assert r.returncode!=0 and (unmanaged/'important').read_text()=='safe'
        passed('installer refuses an unowned nonempty directory')
        link=tmp/'symlink';link.symlink_to(home,target_is_directory=True)
        r=subprocess.run(['sh',str(ROOT/'install.sh'),'--bin-dir',str(a.bolt.resolve().parent),'--prefix',str(link/'bolt')],env=env,capture_output=True,text=True)
        assert r.returncode!=0 and not (home/'bolt').exists()
        passed('installer refuses symbolic-link path traversal')
        standalone=tmp/'without-path'
        r=subprocess.run(['sh',str(ROOT/'install.sh'),'--bin-dir',str(a.bolt.resolve().parent),'--prefix',str(standalone),'--no-path'],env=env,capture_output=True,text=True)
        assert r.returncode==0 and all('Bolt PATH' not in Path(f).read_text() for f in before),r.stderr
        passed('no-path option leaves startup profiles untouched')
        if sys.platform == 'darwin':
            from importlib.util import module_from_spec, spec_from_file_location
            from unittest.mock import patch
            spec = spec_from_file_location('bolt_posix_setup', ROOT / 'tools/install-posix.py')
            installer = module_from_spec(spec)
            spec.loader.exec_module(installer)
            with tempfile.TemporaryDirectory(dir='/tmp') as alias:
                requested = Path(alias) / 'Bolt prefix'
                assert installer.validate_prefix(requested, True) == requested.resolve()
                user_link = Path(alias) / 'user-link'
                user_link.symlink_to(home, target_is_directory=True)
                try:
                    installer.validate_prefix(user_link / 'bolt', True)
                except ValueError:
                    pass
                else:
                    raise AssertionError('accepted a user-created symlink below the system alias')
            passed('Darwin temporary prefix is accepted and a nested user-created symlink is refused')
            registration = {'platform': 'darwin', 'bundle': str(prefix / 'share/bolt/BoltFileIcon.app')}
            for command in (None, Path('/usr/bin/false')):
                with patch.object(installer, '_lsregister', return_value=command):
                    assert installer.register_macos_file_icons(prefix, None) is None
                    assert installer.register_macos_file_icons(prefix, registration) == registration
            passed('missing or unsuccessful LaunchServices registration preserves the managed installation')
    subprocess.run([sys.executable, ROOT / 'tests/macos_install_selection.py'], check=True)
    passed('macOS bundle selection and retained source-install ownership')
    print(f'{count} POSIX installation checks passed')
if __name__=='__main__':main()
