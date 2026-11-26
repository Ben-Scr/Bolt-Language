#!/usr/bin/env python3
"""Direct-backend CLI, linker, deployment, cache and dependency-boundary checks."""
from __future__ import annotations
import argparse
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--bolt',type=Path,required=True);a=p.parse_args();bolt=a.bolt.resolve();count=0
    def passed(name):
        nonlocal count
        count+=1;print('PASS '+name,flush=True)
    def run(*args,env=None):return subprocess.run([str(bolt),*map(str,args)],capture_output=True,text=True,timeout=90,env=env)
    listing=run('backends').stdout
    fast='Fast: available' in listing;llvm='LLVM: not available:' not in listing
    if not fast and not llvm:print('SKIP: no available native backend');return
    backend='fast' if fast else 'llvm'
    with tempfile.TemporaryDirectory(prefix='bolt-native-cli-') as temporary:
        tmp=Path(temporary);project=tmp/'Native ü project';project.mkdir();source=project/'Program.bolt'
        source.write_text('static class Program {public:void Main(){Print((string)42);}}\n')
        r=run('compile',project,'--backend',backend);assert r.returncode==0,r.stdout+r.stderr
        target=project/'bin/Debug'/('Native ü project'+('.exe' if os.name=='nt' else ''))
        assert subprocess.check_output([str(target)],text=True)=='42\n'
        assert not Path(str(target)+'.bolt.cpp').exists();passed('folder compile creates a working native binary without generated C++')
        r=run('compile',project,'--backend',backend);assert r.returncode==0 and 'Up to date' in r.stdout,r.stdout+r.stderr
        passed('unchanged native builds use the checked output cache')
        original=target.read_bytes();source.write_text('static class Program {public:void Main(){int n="bad";}}\n')
        r=run('compile',project,'--backend',backend);assert r.returncode!=0 and target.read_bytes()==original
        passed('semantic failure preserves the previous executable')
        source.write_text('static class Program {public:void Main(){Print(42);}}\n')
        r=run('compile',project,'--backend',backend,'--linker',tmp/'missing-linker','--rebuild');assert r.returncode!=0 and target.read_bytes()==original
        passed('missing native linker is diagnosed without destroying the previous binary')
        r=run('compile',project,'--release','--llvm',tmp/'not-an-llvm-library','--rebuild','-o',target)
        assert r.returncode!=0 and 'LLVM' in r.stderr and target.read_bytes()==original
        passed('missing LLVM is an explicit error, never silent C++ fallback')
        ir=tmp/'Program.ll';r=run('emit-ir',project,'-o',ir);assert r.returncode==0 and 'define ' in ir.read_text() and 'target triple' in ir.read_text()
        passed('native LLVM-readable IR can be inspected')
        obj=project/'bin/Debug/object-only.o';r=run('compile',project,'--backend',backend,'--object-only','--runtime-dir',tmp/'absent','-o',obj)
        assert r.returncode==0 and obj.stat().st_size>64,r.stdout+r.stderr
        passed('object-only code generation does not require the runtime shared library or linker')
        if llvm:
            ecs=tmp/('ecs.exe' if os.name=='nt' else 'ecs')
            r=run('run',ROOT/'tests/native/runtime-free-memory-ecs.bolt','--release','--require-native','-o',ecs)
            assert r.returncode==0,r.stdout+r.stderr
            r=run('run',ROOT/'tests/native/runtime-free-memory-ecs.bolt','--release','--no-compile','--no-runtime','-o',ecs)
            assert r.returncode==0,r.stdout+r.stderr
            passed('Native selection builds the source ECS and ownership implementations together')
            hosted_ecs=tmp/'HostedEcs.bolt'
            hosted_ecs.write_text('import Ecs\nstatic class Program { public: int Main() { Registry world; world.Create(); '
                                  'int n=Runtime.ExecuteProgram("static class Program {public:int Main(){return 7;}}"); '
                                  'return world.EntityCount==1 && n==7 ? 0 : 1; }}\n',encoding='utf-8')
            r=run('run',hosted_ecs,'--release','--require-native','-o',tmp/'hosted-ecs')
            assert r.returncode==0,r.stdout+r.stderr
            passed('The source ECS can share a native executable with hosted runtime calls')
            path_exe=tmp/('paths.exe' if os.name=='nt' else 'paths')
            r=run('run',ROOT/'tests/native/path-components.bolt','--release','--require-native','-o',path_exe)
            assert r.returncode==0,r.stdout+r.stderr
            passed('Bolt path operations preserve platform roots, separators, extensions and Unicode')
            # Supply only the OS startup object on ELF. There is deliberately no
            # Bolt static archive, import library or runtime DLL in this SDK.
            minimal=tmp/'minimal-sdk';minimal.mkdir()
            apps=tmp/'runtime-free-apps';apps.mkdir()
            arch={'amd64':'x64','x86_64':'x64','arm64':'arm64','aarch64':'arm64'}.get(platform.machine().lower(),'')
            imports=ROOT/'bin/windows-libs'/arch
            system_libs=['--sysroot',imports] if os.name=='nt' and (imports/'shell32.lib').exists() else []
            start=bolt.parent/'BoltStart.o'
            if start.is_file():shutil.copy2(start,minimal)
            auto_threads=apps/('threads-auto.exe' if os.name=='nt' else 'threads-auto')
            r=run('compile',ROOT/'tests/native/runtime-free-threading.bolt','--runtime-dir',minimal,
                  *system_libs,'-o',auto_threads)
            assert r.returncode==0,r.stdout+r.stderr
            subprocess.run([auto_threads],cwd=minimal,timeout=20,check=True)
            system_source=tmp/'SystemDefault.bolt'
            system_source.write_text('import System\nimport System.Net\nstatic class Program { public: int Main(string[] args){\n'
                                     'if(args.Length==0)return 42;\n'
                                     'Process child=Process.Start(args[0],new string[0],"",true);\n'
                                     'return child.Capture(5000).ExitCode==42 && Dns.Resolve("localhost").Length>0 ? 0 : 1; }}\n')
            system_app=apps/('systems-auto.exe' if os.name=='nt' else 'systems-auto')
            r=run('compile',system_source,'--release','--runtime-dir',minimal,*system_libs,'-o',system_app)
            assert r.returncode==0,r.stdout+r.stderr
            subprocess.run([system_app,system_app],cwd=minimal,timeout=20,check=True)
            passed('Default process and DNS builds run using only system libraries')
            (minimal/'empty').write_bytes(b'')
            (minimal/'ü😀.txt').write_bytes('Grüße\0😀\n'.encode('utf-8-sig'))
            (minimal/'plain').write_bytes(b'first\r\nlast')
            (minimal/'bom-only').write_bytes(b'\xef\xbb\xbf')
            (minimal/'invalid-utf8').write_bytes(b'\xc3(')
            (minimal/'binary').write_bytes(bytes(range(256))*96)
            if os.name!='nt':
                with (minimal/'too-large').open('wb') as sparse:sparse.truncate(1<<31)
            cases={'native/path-components':'','native/scalar-math':'','native/runtime-free-numeric-parsing':'','native/runtime-free-collection-values':'','native/runtime-free-values':'','native/runtime-free-systems':'',
                   'native/runtime-free-float-formatting':'','native/runtime-free-integer-formatting':'',
                   'native/runtime-free-bool-char-formatting':'',
                   'native/runtime-free-file-read':'','native/runtime-free-file-write':'',
                   'native/runtime-free-memory-ecs':'','systems14/Memory':'MEMORY14:46\n',
                   'native/runtime-free-file-lines':'','systems14/Diagnostics':'DIAGNOSTICS14:passed\n',
                   'native/runtime-free-threading':'','native/runtime-free-continuations':'',
                   'fixtures/HostedTaskCompletion':'','systems14/Threading':'THREADING14:24\n',
                   'native/array-range-replacement':'true\n','native/utf8-replacement':'true\n',
                   'native/buffer-scalar-append':'','native/copied-memory-compare':'','native/scalar-list-shifts':'',
                   'native/array-loop-versioning':'','native/checked-array-reduction':'',
                   'native/string-equality-widths':'true\n','portable/exceptions':'portable-exceptions-ok\n'}
            if os.name == "nt" or platform.system() in ("Linux", "Darwin", "FreeBSD"):
                cases.update(dict.fromkeys(('native/runtime-free-streams', 'native/stream-read-remaining'), ''))
            if os.name == "nt" or platform.system() == "Linux":
                cases.update(dict.fromkeys(('native/runtime-free-filesystem', 'native/runtime-free-file-async'), ''))
            if os.name == 'nt' or platform.system() == 'Linux':
                cases.update({
                    'native/runtime-free-reflection-json': '',
                    'features/variadic-reflection': '4\n12\n7\n7\nref!\n18\n',
                    'nine/PropertyReflection': '7\nBolt\n12\n24\n12\n21\n0\n5\ntrue\ntrue\n0\n',
                })
            for case in json.loads((ROOT/'tests/arena_cases.json').read_text()):
                if 'error' not in case and case['file'].endswith('.bolt'):
                    cases[str(Path(case['file']).relative_to('tests').with_suffix('').as_posix())]=case['stdout']
            interface_cases={'native/runtime-free-delegates':'','features15/Events':'static=4\n10\n15\n',
                             'features/closures-switch':'closures and switch OK\n'}
            for case in json.loads((ROOT/'tests/v5_cases.json').read_text()):
                fixture=Path(case['file']).stem
                if fixture in ('boxed-collections','interface-box-copy','interface-explicit-cast','base-interface',
                               'collection-stringable-interface','collection-stringable-nested'):
                    interface_cases['v5/'+fixture]=case['stdout']
            cases.update(interface_cases)
            for fixture,expected in cases.items():
                for profile in ('--debug', '--release'):
                    exe=apps/(Path(fixture).name+profile+('.exe' if os.name=='nt' else ''))
                    r=run('compile',ROOT/f'tests/{fixture}.bolt',profile,'--no-runtime',
                          '--runtime-dir',minimal,*system_libs,'-o',exe)
                    assert r.returncode==0,r.stdout+r.stderr
                    # /dev/stdin supplies a non-seekable pipe to the POSIX file reader.
                    pipe_input='s'*24576+'end' if fixture=='native/runtime-free-file-read' and os.name!='nt' else None
                    work=minimal
                    if fixture in ('native/runtime-free-file-write','native/runtime-free-file-lines','native/runtime-free-filesystem',
                                   'native/runtime-free-streams','native/runtime-free-file-async'):
                        work=tmp/(Path(fixture).name+profile)
                        work.mkdir()
                    if fixture == 'native/runtime-free-file-async' and platform.system() == 'Linux':
                        os.mkfifo(work/'waiting-pipe')
                    if fixture == 'native/runtime-free-filesystem':
                        keep, links = work/'keep', work/'links'
                        keep.mkdir()
                        links.mkdir()
                        (keep/'marker').write_text('preserve')
                        if os.name == 'nt':
                            import _winapi
                            _winapi.CreateJunction(str(keep), str(links/'outside'))
                        else:
                            (links/'outside').symlink_to(keep, target_is_directory=True)
                    command=[exe,exe] if fixture=='native/runtime-free-systems' else [exe]
                    execution=subprocess.run(command,cwd=work,input=pipe_input,capture_output=True,text=True,
                                             encoding='utf-8' if fixture=='systems14/Diagnostics' else None,timeout=20)
                    assert execution.returncode==0 and execution.stdout==expected, (
                        f'{fixture} {profile} returned {execution.returncode}\n'+execution.stdout+execution.stderr)
                    if fixture=='systems14/Diagnostics':
                        assert execution.stderr=='TRACE14:real-stderr\nTRACE14:Grüße\x00😀\n42\n',repr(execution.stderr)
                    if fixture=='native/runtime-free-streams':
                        assert (work/'data').read_bytes() == b'oneA'
                    if fixture=='native/runtime-free-file-async':
                        assert (work/'joined-on-exit').read_text() == 'done'
                    if fixture=='native/runtime-free-file-write':
                        assert (work/'Daten-ü😀.bin').read_bytes()==b'\xff\x00\x80'
                        assert (work/'Grüße-😀.txt').read_bytes()=='Grüße\0😀\r\n'.encode('utf-8')
                        assert (work/'empty-text').read_bytes()==(work/'empty-bytes').read_bytes()==b''
                    if fixture=='native/runtime-free-file-lines':
                        assert (work/'Zeilen-ü😀.txt').read_bytes()=='Grüße😀\n\nA\0B\noffen+Ende\n\nΩ\n'.encode('utf-8')
                        assert (work/'opaque').read_bytes()==b'\xff\x00\xc3\xa9x\n'
                    if (fixture in interface_cases or fixture in ('native/runtime-free-memory-ecs','systems14/Memory','systems14/Diagnostics','native/runtime-free-systems','native/runtime-free-threading','native/runtime-free-continuations','fixtures/HostedTaskCompletion','systems14/Threading','native/path-components','native/runtime-free-filesystem','native/runtime-free-streams',
                                    'native/scalar-math','native/runtime-free-numeric-parsing','native/runtime-free-collection-values','native/runtime-free-float-formatting','native/runtime-free-integer-formatting','native/runtime-free-bool-char-formatting','native/runtime-free-reflection-json','features/variadic-reflection','nine/PropertyReflection') or fixture.startswith('native/runtime-free-file-')) and profile=='--release':
                        inspect=shutil.which('dumpbin') if os.name=='nt' else shutil.which('readelf')
                        if inspect:
                            option='/dependents' if os.name=='nt' else '-d'
                            deps=subprocess.check_output([inspect,option,exe],text=True).lower()
                            assert not any(lib in deps for lib in ('boltruntime','libstdc++','libc++','msvcp','vcruntime')),deps
            passed('Tasks, worker callbacks, typed failures and synchronization run without a Bolt runtime in Debug and Release')
            automatic=apps/('delegates-default'+('.exe' if os.name=='nt' else ''))
            r=run('compile',ROOT/'tests/native/runtime-free-delegates.bolt','--runtime-dir',minimal,*system_libs,'-o',automatic)
            assert r.returncode==0,r.stdout+r.stderr
            r=run('run',ROOT/'tests/native/runtime-free-delegates.bolt','--no-compile','--no-runtime','-o',automatic)
            assert r.returncode==0,r.stdout+r.stderr
            passed('Interface boxing, collection keys, closures and events run without the Bolt runtime')
            contracts=tmp/'Contracts.bolt'
            program=('import System.Collections.Generic\ninterface IKey {int Read();}\n'
                     'class Key:IKey {public: int Read(){return 42;} '
                     '[ReadOnly] ulong GetHashCode(){return (ulong)Runtime.ExecuteProgram("unused");}}\n'
                     'static class Program {public: int Main(){IKey key=Key();')
            contracts.write_text(program+'return key.Read();}}')
            r=run('compile',contracts,'--no-runtime','--runtime-dir',minimal,*system_libs,'-o',automatic)
            assert r.returncode==0 and subprocess.run([automatic],timeout=20).returncode==42,r.stdout+r.stderr
            contracts.write_text(program+'HashSet<IKey> keys;keys.Add(key);return keys.Count;}}')
            r=run('compile',contracts,'--no-runtime','--runtime-dir',minimal,*system_libs,'-o',automatic)
            assert r.returncode!=0 and 'Key.GetHashCode calls br_runtime' in r.stderr,r.stdout+r.stderr
            if 'native/runtime-free-reflection-json' in cases:
                automatic=apps/('metadata-default'+('.exe' if os.name=='nt' else ''))
                r=run('compile',ROOT/'tests/native/runtime-free-reflection-json.bolt','--release',
                      '--runtime-dir',minimal,*system_libs,'-o',automatic)
                assert r.returncode==0,r.stdout+r.stderr
                r=run('run',ROOT/'tests/native/runtime-free-reflection-json.bolt','--release',
                      '--no-compile','--no-runtime','-o',automatic)
                assert r.returncode==0,r.stdout+r.stderr
                passed('Default release builds select runtime-free JSON and reflection')
            if 'native/runtime-free-filesystem' in cases:
                passed('JSON text, BJS1, type metadata and dynamic calls run without a Bolt runtime')
                passed('Runtime-free filesystem operations preserve overwrite rules and directory link targets')
                passed('Streams, readers, writers and asynchronous file jobs run without a Bolt runtime')
            passed('File.ReadAllBytes and File.ReadAllText run without a Bolt runtime in Debug and Release')
            passed('Runtime-free byte and UTF-8 writes create, truncate and round-trip in Debug and Release')
            passed('Runtime-free line reads, writes and appends preserve UTF-8, empty lines and file contents')
            failure=tmp/'Uncaught.bolt'
            failure.write_text('import System\nstatic class Program { public: void Main(){\n'
                               'throw InvalidOperationException("uncaught-test");\n} }\n')
            failed_exe=apps/('uncaught'+('.exe' if os.name=='nt' else ''))
            r=run('compile',failure,'--release','--no-runtime','--runtime-dir',minimal,'-o',failed_exe)
            assert r.returncode==0,r.stdout+r.stderr
            failure.unlink()
            r=subprocess.run([failed_exe],cwd=minimal,capture_output=True,text=True,timeout=20)
            assert r.returncode!=0 and all(part in r.stderr for part in (
                'InvalidOperationException','uncaught-test','Uncaught.bolt:3:','throw InvalidOperation','at Program.Main')),r.stderr
            passed('Path, Unicode strings, array copies, catch/rethrow and finally run without a Bolt runtime')
            argument_source=tmp/'Arguments.bolt'
            argument_source.write_text('static class Program { public: int Main(string[] args) { '
                'return args.Length==3 && args[0]=="a b" && args[1]=="ä😀" && args[2]=="" ? 0 : 1; } }',
                encoding='utf-8')
            argument_exe=tmp/('arguments'+('.exe' if os.name=='nt' else ''))
            r=run('compile',argument_source,'--release','--no-runtime','--runtime-dir',minimal,*system_libs,'-o',argument_exe)
            assert r.returncode==0,r.stdout+r.stderr
            r=subprocess.run([argument_exe,'a b','ä😀',''],cwd=minimal,capture_output=True,text=True,timeout=20)
            assert r.returncode==0,r.stderr
            passed('runtime-free Main(string[]) preserves Unicode, quoted and empty process arguments')
            if os.name=='nt' and arch=='x64':
                large=tmp/'StackPages.bolt'
                fields='\n'.join(f'long f{i};' for i in range(1025))
                large.write_text('struct Slab {public:\n'+fields+'\n}\nstatic class Program {public: '
                    'int Read(Slab& s){s.f0=2l;s.f1024=40l;return (int)(s.f0+s.f1024);} '
                    'int Main(){Slab s;return Read(s)==42 ? 0 : 1;} }')
                r=run('compile',large,'--debug','--no-runtime','--runtime-dir',minimal,*system_libs,'-o',argument_exe)
                assert r.returncode==0,r.stdout+r.stderr
                assert subprocess.run([argument_exe],cwd=minimal,timeout=20).returncode==0
                passed('Windows x64 initializes and probes multi-page stack frames without CRT libraries')
            pure=tmp/'os-call.bolt'
            shutil.copyfile(ROOT/'tests/native/runtime-free.bolt',pure)
            pure_exe=tmp/('os-call.exe' if os.name=='nt' else 'os-call')
            def minimal_build(source):
                return run('compile',source,'--release','--backend','llvm','--require-native',
                           '--runtime-dir',minimal,'-o',pure_exe)
            r=minimal_build(pure)
            assert r.returncode==0,r.stdout+r.stderr
            assert subprocess.run([pure_exe],timeout=20).returncode==27
            r=minimal_build(pure)
            assert r.returncode==0 and 'Up to date' in r.stdout,r.stdout+r.stderr
            r=minimal_build(project)
            assert r.returncode!=0 and 'native runtime not found' in r.stderr,r.stdout+r.stderr
            assert subprocess.run([pure_exe],timeout=20).returncode==27
            passed('OS imports and Thread.Yield link without BoltRuntime; runtime-dependent builds preserve the output')
            effect=tmp/'initializer.bolt'
            effect.write_text('static class Program { static string owned = "first"; public: int Main(){ '
                              'if(owned != "first") return 2; owned += "!"; return owned == "first!" ? 0 : 3; } }')
            r=run('run',effect,'--release','--no-runtime','-o',tmp/('statics'+('.exe' if os.name=='nt' else '')))
            assert r.returncode==0,r.stdout+r.stderr
            assert subprocess.run([pure_exe],timeout=20).returncode==27
            passed('runtime-free executables initialize and release owned static strings')
            r=run('compile',pure,'--release','--no-runtime','-o',pure_exe)
            assert r.returncode==0,r.stdout+r.stderr
            r=run('run',pure,'--release','--no-runtime','--no-compile','-o',pure_exe)
            assert r.returncode==27,r.stdout+r.stderr
            destructor=tmp/'hosted-destructor.bolt'
            destructor.write_text('class Hosted {public: ~Hosted(){Runtime.ExecuteProgram("invalid");}} '
                                  'static class Program {public: void Main(){Hosted* p=new Hosted(); delete p;}}')
            r=run('compile',destructor,'--release','--no-runtime','-o',pure_exe)
            assert r.returncode!=0 and 'Hosted.~Hosted calls br_runtime' in r.stderr,r.stdout+r.stderr
            hosted=tmp/'hosted.bolt'
            hosted.write_text('static class Program { public: int Main(){ return Runtime.ExecuteProgram("invalid"); } }')
            r=run('compile',hosted,'--release','--no-runtime','-o',pure_exe)
            assert r.returncode!=0 and '--no-runtime' in r.stderr,r.stdout+r.stderr
            assert subprocess.run([pure_exe],timeout=20).returncode==27
            hosted_exe=tmp/('hosted-release'+('.exe' if os.name=='nt' else ''))
            r=run('compile',hosted,'--release','-o',hosted_exe)
            assert r.returncode==0,r.stdout+r.stderr
            r=run('run',hosted,'--release','--no-compile','--no-runtime','-o',hosted_exe)
            assert r.returncode!=0 and '--no-runtime' in r.stderr,r.stdout+r.stderr
            floating_source=tmp/'floating.bolt'
            floating_source.write_text('''import System
static class Program {
    public:void Main(){
        Print((1.25f+3f).ToString());
        Print(Converter.ToString(2.5*2));
    }
}
''')
            floating_exe=tmp/('floating-release'+('.exe' if os.name=='nt' else ''))
            r=run('compile',floating_source,'--backend','llvm','--profile','Release','--require-native','-o',floating_exe)
            assert r.returncode==0,r.stdout+r.stderr
            r=subprocess.run([str(floating_exe)],capture_output=True,text=True,timeout=20)
            assert r.returncode==0 and r.stdout=='4.25\n5\n',r.stdout+r.stderr
            r=run('run',floating_source,'--release','--no-compile','--no-runtime','-o',floating_exe)
            assert r.returncode==0 and r.stdout=='4.25\n5\n',r.stdout+r.stderr
            passed('--no-runtime enforces the dependency check on builds and saved executables')
            passed('LLVM Release AOT links and executes float32/float64 arithmetic')
        if llvm and os.name=='nt' and (bolt.parent/'BoltAotRuntime.lib').is_file() and os.environ.get('VCToolsInstallDir') and os.environ.get('WindowsSdkDir'):
            standalone=tmp/'Standalone';standalone.mkdir()
            built=standalone/'program.exe'
            r=run('compile',project,'--release','--require-native','-o',built)
            assert r.returncode==0,r.stdout+r.stderr
            assert not (standalone/'BoltRuntime.dll').exists()
            assert subprocess.check_output([str(built)],text=True,cwd=standalone)=='42\n'
            passed('Windows x64 Release runs from an isolated directory with a statically linked runtime')
            pure=tmp/'Pure.bolt'
            pure.write_text('static class Program {public:int Main(){return 27;}}')
            r=run('compile',pure,'--release','--require-native','-o',built)
            assert r.returncode==0,r.stdout+r.stderr
            assert subprocess.run([str(built)],cwd=standalone,timeout=20).returncode==27
            passed('Windows x64 runtime-free entry preserves the program exit status')
            dynamic=tmp/'Runtime.bolt'
            dynamic.write_text('static class Program {public:void Main(){Print(Runtime.ExecuteProgram("static class Program {public:int Main(){return 33;}}"));}}')
            r=run('compile',dynamic,'--release','--require-native','-o',built)
            assert r.returncode==0 and (standalone/'BoltRuntime.dll').is_file(),r.stdout+r.stderr
            assert subprocess.check_output([str(built)],text=True,cwd=standalone)=='33\n'
            passed('Windows x64 Release deploys the full runtime for dynamic source execution')
        if llvm and os.name!='nt' and os.uname().sysname=='Darwin':
            release_folder=tmp/'Mac Release';release_folder.mkdir()
            built=release_folder/'program'
            r=run('compile',project,'--release','--require-native','-o',built)
            assert r.returncode==0,r.stdout+r.stderr
            listing=subprocess.check_output(['/usr/bin/otool','-L',str(built)],text=True)
            assert 'libBoltRuntime' not in listing,listing
            assert not (release_folder/'libBoltRuntime.dylib').exists()
            independent=tmp/'Independent Mac App';independent.mkdir()
            app=independent/'program';shutil.copy2(built,app)
            assert subprocess.check_output([str(app)],cwd=independent)==b'42\n'
            passed('macOS Release Mach-O runs after moving the executable alone')
        if fast:
            env=os.environ.copy();env['PATH']='';r=run('exec','Print(6*7);','--jit','--backend','fast',env=env)
            assert r.returncode==0 and r.stdout=='42\n',r.stdout+r.stderr
            passed('fast JIT executes with an empty PATH and no subprocess compiler/linker')
        if os.name!='nt' and os.uname().sysname=='Linux':
            linker=shutil.which('ld.lld') or shutil.which('ld');assert linker
            traps=tmp/'compiler-traps';traps.mkdir();marker=tmp/'compiler-was-invoked'
            for name in ['c++','g++','gcc','clang','clang++','cc']:
                script=traps/name;script.write_text('#!/bin/sh\nprintf called > '+str(marker)+'\nexit 91\n');script.chmod(0o755)
            env=os.environ.copy();env['PATH']=str(traps)
            r=run('compile',project,'--backend',backend,'--rebuild','--linker',linker,env=env)
            assert r.returncode==0 and not marker.exists(),r.stdout+r.stderr
            passed('direct AOT links successfully with all external C/C++ compilers trapped')
            if llvm:
                release=tmp/'release-direct';r=run('compile',project,'--release','--linker',linker,'-o',release,env=env)
                assert r.returncode==0 and not marker.exists() and subprocess.check_output([str(release)])==b'42\n',r.stdout+r.stderr
                passed('LLVM release compiles without invoking clang/GCC/C++')
                if (bolt.parent/'BoltAotRuntime.o').is_file():
                    isolated=tmp/'standalone-release';isolated.mkdir()
                    deployed=isolated/'app';shutil.copy2(release,deployed)
                    listing=subprocess.check_output(['ldd',str(deployed)],text=True)
                    assert 'libBoltRuntime' not in listing and 'libstdc++' not in listing,listing
                    assert subprocess.check_output([str(deployed)],cwd=isolated,env=env)==b'42\n'
                    passed('deployed GNU/Linux Release ELF runs without a companion Bolt runtime')
            gnu=shutil.which('ld')
            if gnu:
                r=run('compile',project,'--backend',backend,'--linker',gnu,'--rebuild');assert r.returncode==0,r.stdout+r.stderr
                passed('GNU ld is also supported as the native linker')
            deployment=tmp/'deployed';deployment.mkdir();shutil.copy2(target,deployment/'app');shutil.copy2(target.parent/'libBoltRuntime.so',deployment/'libBoltRuntime.so')
            env=os.environ.copy();env['PATH']='';env['BOLT_LLVM_LIBRARY']=str(tmp/'absent-llvm')
            r=subprocess.run([str(deployment/'app')],capture_output=True,text=True,env=env,cwd=deployment,timeout=20)
            assert r.returncode==0 and r.stdout=='42\n',r.stdout+r.stderr
            passed('ordinary deployed native programs need no Bolt compiler or LLVM installation')
            r=run('compile',project,'--backend','llvm','--debug','-o',tmp/'debug-lines') if llvm else None
            if r is not None:
                assert r.returncode==0,r.stdout+r.stderr
                readelf=shutil.which('readelf')
                if readelf:
                    info=subprocess.check_output([readelf,'--debug-dump=decodedline',str(tmp/'debug-lines')],text=True)
                    assert 'Program.bolt' in info
                    passed('LLVM debug objects map native addresses to .bolt source lines')
        error_source=tmp/'error.bolt';error_source.write_text('static class Program {\npublic:\nvoid Main(){\nint zero=0;\nPrint(1/zero);\n}\n}\n')
        error_exe=tmp/('error.exe' if os.name=='nt' else 'error-native')
        r=run('compile',error_source,'--backend',backend,'-o',error_exe);assert r.returncode==0,r.stderr
        error_source.unlink();r=subprocess.run([str(error_exe)],capture_output=True,text=True,timeout=20)
        assert r.returncode!=0 and 'Print(1/zero)' in r.stderr and ':5:' in r.stderr,r.stderr
        passed('native runtime diagnostic carries the source line after the source file is removed')
        note=target.parent/'keep.txt';note.write_text('user data');r=run('clean',project)
        assert r.returncode==0 and not target.exists() and not obj.exists() and note.read_text()=='user data',r.stdout+r.stderr
        passed('clean removes marked native executable/object outputs and preserves unrelated files')
    print(f'{count} native CLI checks passed')
if __name__=='__main__':main()
