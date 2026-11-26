#!/usr/bin/env python3
"""Native-default CLI integration tests. All programs/artifacts live in a temp folder.
Exercises real code generation/linking/execution, not a mocked command dispatcher.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt',type=Path,required=True)
    parser.add_argument('--cxx',default='cl.exe' if os.name=='nt' else 'c++')
    args=parser.parse_args(); bolt=str(args.bolt.resolve()); count=0
    def ok(label):
        nonlocal count
        count+=1;print('PASS '+label,flush=True)
    with tempfile.TemporaryDirectory(prefix='bolt010-run-') as directory:
        work=Path(directory); project=work/'Project space ü'; project.mkdir()
        source=project/'Program.bolt'
        def invoke(*argv,code=0,cwd=project,env=None,timeout=180):
            out=subprocess.run([bolt,*map(str,argv)],cwd=cwd,env=env,capture_output=True,text=True,encoding='utf-8',timeout=timeout)
            if out.returncode!=code:
                raise AssertionError(f'{argv}: exit {out.returncode}, expected {code}\n{out.stdout}\n{out.stderr}')
            return out
        def text(label,exit_code=7):
            return f'''public static class Program {{ public: int Main(string[] args) {{
Print("{label}"); Print(args.Length);
for(int i=0;i<args.Length;i++) {{ Print(args[i]); }}
return {exit_code}; }} }}\n'''
        def target(configuration='Debug'):
            return project/'bin'/configuration/(project.name+('.exe' if os.name=='nt' else ''))
        def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
        caps=invoke('backends').stdout
        fast='Fast: available' in caps
        host=next(line.removeprefix('Host: ') for line in caps.splitlines() if line.startswith('Host: '))
        llvm='LLVM: not available:' not in caps
        direct='fast' if fast else 'llvm'
        if not fast and not llvm:
            raise AssertionError('A direct backend is required for native-default regression coverage')
        source.write_text(text('debug-v1'),encoding='utf-8')
        missing=invoke('run','--no-compile',code=1)
        assert 'No previous Debug' in missing.stderr and not (project/'bin').exists()
        ok('missing --no-compile build fails without creating or compiling an executable')
        app_args=['one','two words','Ä😀','','--release','quote"value','back\\slash']
        expected='debug-v1\n7\n'+'\n'.join(app_args)+'\n'
        out=invoke('run','.','--require-native','--',*app_args,code=7)
        assert target().is_file() and out.stdout.endswith(expected),out.stdout
        assert 'Debug compilation:' in out.stderr and 'interpreter preparation' not in out.stderr
        assert ('Bolt Fast '+host) in out.stdout if fast else 'LLVM' in out.stdout, out.stdout
        assert not list(project.rglob('*.bolt.cpp'))
        ok('plain run is actual native Debug AOT, forwards argv and returns the child exit code')
        old_binary=digest(target()); old_mtime=target().stat().st_mtime_ns
        out=invoke('run',code=7)
        assert 'Up to date' in out.stdout and 'cache hit' in out.stderr
        assert target().stat().st_mtime_ns==old_mtime
        ok('default-path run reuses the existing checked build cache without relinking')
        out=invoke('compile')
        assert 'Debug compilation:' in out.stderr and 'debug-v1\n0\n' not in out.stdout
        out=invoke('build','--no-timings')
        assert not re.search(r'\bms\b',out.stderr)
        ok('compile/build print time by default; --no-timings suppresses it and neither runs Main')
        # No compiler, linker, LLVM or source loading may be necessary for a saved run.
        isolated=dict(os.environ);isolated['PATH']='';isolated['BOLT_LLVM_LIBRARY']=str(work/'missing-llvm')
        isolated['BOLT_LINKER']=str(work/'missing-linker')
        source.write_text('this is deliberately invalid source !!!',encoding='utf-8')
        out=invoke('run','.','--no-compile','--','saved argument',code=7,env=isolated)
        assert out.stdout=='debug-v1\n1\nsaved argument\n' and 'Compilation skipped' in out.stderr
        assert 'compilation:' not in out.stderr and digest(target())==old_binary
        ok('--no-compile ignores invalid edited source and needs no compiler/linker/LLVM in PATH')
        source.unlink()
        out=invoke('run','.','--no-compile',code=7,env=isolated)
        assert out.stdout=='debug-v1\n0\n'
        ok('--no-compile does not even require project source files to remain present')
        source.write_text('static class Program {public: void Main(){ int bad = ; }}',encoding='utf-8')
        failed=invoke('run',code=1)
        assert 'Build/preparation failed after:' in failed.stderr and digest(target())==old_binary
        assert invoke('run','--no-compile',code=7).stdout=='debug-v1\n0\n'
        ok('a failed frontend build does not lose the previous executable or its receipt')
        source.write_text(text('debug-v2'),encoding='utf-8')
        invoke('run','.','--backend',direct,'--linker',work/'missing-linker','--rebuild',code=1)
        assert digest(target())==old_binary
        assert invoke('run','--no-compile',code=7).stdout=='debug-v1\n0\n'
        ok('a failed link does not replace the last successful build')
        invoke('run','.','--rebuild','--require-native',code=7)
        assert invoke('run','--no-compile',code=7).stdout=='debug-v2\n0\n'
        ok('a successful rebuild updates the last-build selection')
        if llvm:
            source.write_text(text('release-v1'),encoding='utf-8')
            out=invoke('run','.','--release','--require-native',code=7)
            assert target('Release').is_file() and 'LLVM' in out.stdout and 'Release compilation:' in out.stderr
            assert out.stdout.endswith('release-v1\n0\n')
            ok('run --release performs optimized LLVM AOT and launches the Release executable')
            source.write_text('invalid again',encoding='utf-8')
            assert invoke('run','--release','--no-compile',code=7,env=isolated).stdout=='release-v1\n0\n'
            assert invoke('run','--no-compile',code=7,env=isolated).stdout=='debug-v2\n0\n'
            ok('Debug and Release saved builds stay distinct; even Release no-compile never loads LLVM')
            source.write_text(text('release-v2'),encoding='utf-8')
            out=invoke('compile','.','--release','--require-native')
            assert 'Release compilation:' in out.stderr
            assert invoke('run','--release','--no-compile',code=7).stdout=='release-v2\n0\n'
            ok('standalone Release compile also registers a runnable last-successful build')
        # A different output is selected when that is the most recently built one.
        source.write_text(text('custom'),encoding='utf-8')
        custom=work/('custom output.exe' if os.name=='nt' else 'custom output')
        invoke('compile','.','-o',custom,'--require-native')
        assert invoke('run','--no-compile',code=7).stdout=='custom\n0\n'
        assert invoke('run','--no-compile','-o',target(),code=7).stdout=='debug-v2\n0\n'
        ok('custom -o is recorded as the last build; explicit -o can select a different recorded output')
        mismatch=invoke('run','--no-compile','--backend','cpp',code=1)
        assert 'last build used --backend' in mismatch.stderr
        ok('--no-compile cannot misrepresent the backend of a saved executable')
        if llvm:
            invoke('compile','.','--release','-o',custom,'--require-native')
            out=invoke('run','--no-compile',code=1)
            assert 'replaced by another build' in out.stderr,out.stderr
            assert invoke('run','--release','--no-compile',code=7).stdout=='custom\n0\n'
            ok('reusing -o for another configuration invalidates the older configuration receipt')
        invoke('compile','.','--rebuild','--require-native')
        binary=target().read_bytes()
        target().write_bytes(binary+b'changed')
        assert 'changed after' in invoke('run','--no-compile',code=1).stderr
        target().write_bytes(binary)
        ok('modified binaries are not silently treated as the last successful build')
        target().unlink()
        assert 'missing or was replaced' in invoke('run','--no-compile',code=1).stderr
        target().write_bytes(binary)
        if os.name!='nt':target().chmod(0o755)
        ok('missing binaries fail clearly rather than auto-compiling')
        runtime_name='BoltRuntime.dll' if os.name=='nt' else ('libBoltRuntime.dylib' if os.uname().sysname=='Darwin' else 'libBoltRuntime.so')
        runtime=target().parent/runtime_name; backup=runtime.with_name(runtime.name+'.backup')
        runtime.rename(backup)
        try: assert 'runtime file' in invoke('run','--no-compile',code=1).stderr
        finally:backup.rename(runtime)
        ok('missing matching runtime is detected before launching a saved direct executable')
        # New modes are genuinely optional and do not create AOT artifacts.
        modes=work/'modes';modes.mkdir();mode_source=modes/'Program.bolt';mode_source.write_text(text('mode',0),encoding='utf-8')
        out=invoke('run','.','--runtime','--','runtime arg','',cwd=modes,env=isolated)
        assert out.stdout=='mode\n2\nruntime arg\n\n' and 'no native compilation' in out.stderr
        assert not (modes/'bin').exists() and not (modes/'.bolt').exists()
        ok('explicit --runtime interprets Main(string[] args) without probing native tools or creating binaries')
        out=invoke('run','.','--jit','--backend',direct,cwd=modes)
        assert out.stdout=='mode\n0\n' and 'JIT compilation:' in out.stderr and not (modes/'bin').exists()
        ok('optional --jit emits native code in memory, not a persistent AOT binary')
        assert 'No previous Debug' in invoke('run','--no-compile',cwd=modes,code=1).stderr
        ok('interpreter/JIT runs are not incorrectly registered as saved AOT builds')
        if llvm:
            out=invoke('run','.','--jit','--release','--require-native',cwd=modes)
            assert out.stdout=='mode\n0\n' and 'JIT compilation:' in out.stderr
            ok('explicit optimized LLVM JIT remains available without changing the AOT default')
        mode_source.write_text('static class Program{public:void Main(){while(true){}}}')
        out=invoke('run','.','--runtime','--steps','30',code=1,cwd=modes)
        assert 'budget' in out.stderr or 'limit' in out.stderr,out.stderr
        ok('interpreter step budget remains available only in an appropriate execution mode')
        invalid=[
            ('run','--runtime','--release'),('run','--runtime','--debug'),('run','--runtime','--jit'),
            ('run','--native','--jit'),('run','--runtime','--native'),('run','--runtime','--backend','fast'),
            ('run','--runtime','--require-native'),('run','--runtime','-o','bad'),
            ('run','--no-compile','--jit'),('run','--no-compile','--runtime'),('run','--no-compile','--rebuild'),
            ('run','--no-compile','--cxx','ignored'),('run','--no-compile','--linker','ignored'),
            ('run','--no-compile','--steps','1'),('compile','--no-compile'),('compile','--runtime'),
            ('run','--debug','--release'),('run','--object-only'),('run','--jit','-o','bad'),
            ('run','--steps','10'),('compile','--steps','10'),('run','--jit','--backend','cpp')]
        for options in invalid:
            out=invoke(*options,code=1,cwd=modes)
            assert 'BLT4000' in out.stderr and 'Build/preparation' not in out.stderr,(options,out.stderr)
        ok(f'{len(invalid)} incompatible mode/option combinations rejected before loading source')
        # File-mode run receipts are distinct from folder-mode receipts.
        lone=work/'single.bolt';lone.write_text(text('single',0),encoding='utf-8')
        out=invoke('run',lone,'--require-native');assert out.stdout.endswith('single\n0\n')
        lone.unlink()
        assert invoke('run',lone,'--no-compile').stdout=='single\n0\n'
        ok('single-file builds have independent last-build records and can run after source removal')
        # Features not yet in direct backends must keep working natively via compatibility.
        fallback=work/'fallback';fallback.mkdir();(fallback/'Program.bolt').write_text(
            '[Header("cstdlib"), Native("std::abs")] extern "C++" int Magnitude(int value); static class Program{public:void Main(){Print(Magnitude(-42));}}')
        out=invoke('run','.','--cxx',args.cxx,'--no-pch',cwd=fallback,timeout=300)
        assert out.stdout.endswith('42\n') and 'Debug compilation:' in out.stderr
        assert invoke('run','--no-compile',cwd=fallback,env=isolated).stdout=='42\n'
        assert '--require-native' in invoke('run','--no-compile','--require-native',code=1,cwd=fallback).stderr
        out=invoke('run','--runtime',code=1,cwd=fallback)
        assert 'No native compilation was attempted' in out.stderr
        ok('C++ fallback builds also get saved-build records; explicit runtime/native-only modes never silently fall back')
        # A program delay is not included in build timing; use a real linked program.
        sleeper=work/'sleeper';sleeper.mkdir();(sleeper/'Program.bolt').write_text(
            'import System.Threading.Tasks\nstatic class Program{public:void Main(){Task.Delay(250).Wait();Print("done");}}')
        out=invoke('run','.','--cxx',args.cxx,cwd=sleeper,timeout=300)
        merged=subprocess.run([bolt,'run','.','--no-compile'],cwd=sleeper,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',timeout=30)
        assert merged.returncode==0 and merged.stdout.index('Compilation skipped')<merged.stdout.index('\ndone\n')
        merged=subprocess.run([bolt,'run','.'],cwd=project,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',timeout=60)
        assert merged.returncode==7 and merged.stdout.index('Debug compilation:')<merged.stdout.index('\ncustom\n')
        ok('build/skip status is flushed before any user program output or waiting')
        # Cleaning must invalidate saved builds without deleting unrelated JSON.
        keep=project/'.bolt/run/notes.json';keep.write_text('{"my":"notes"}')
        out=invoke('clean',project)
        assert keep.read_text()=='{"my":"notes"}'
        assert 'No previous Debug' in invoke('run','--no-compile',code=1).stderr
        ok('clean removes owned receipts while preserving unrelated user JSON')
        for command in ('run','compile'):
            help_text=invoke('help',command).stdout
            assert 'default' in help_text.lower() and 'tim' in help_text
        assert '--no-compile' in invoke('help','run').stdout and '--runtime' in invoke('help','run').stdout
        ok('CLI help describes actual default execution, modes, saved builds and timing')
    print(f'{count} native-default CLI test groups passed',flush=True)
    return 0

if __name__=='__main__':raise SystemExit(main())
