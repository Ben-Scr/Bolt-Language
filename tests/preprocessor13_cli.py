#!/usr/bin/env python3
"""Real profile/define/cache/import workflows, including independent library defines."""
import argparse,json,pathlib,subprocess,tempfile,os
P=pathlib.Path
p=argparse.ArgumentParser();p.add_argument('--bolt',required=True,type=P);p.add_argument('--cxx',default='');a=p.parse_args();B=str(a.bolt.resolve());count=0

def check(ok,msg):
 global count
 if not ok:raise AssertionError(msg)
 count+=1;print('PASS '+msg,flush=True)
def run(*args,ok=True,cwd=None):
 r=subprocess.run([B,*map(str,args)],cwd=cwd,text=True,capture_output=True,timeout=180)
 if ok and r.returncode:raise AssertionError(str(args)+'\n'+r.stdout+r.stderr)
 return r

def write(path,text):path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='bolt-preprocessor13-') as d:
 root=P(d);proj=root/'Project';run('init',proj)
 file=proj/'src/Main.bolt'
 write(file,'''#if false
import "unavailable-platform.bolt"
@ invalid disabled source !
#endif
static class Program {public:void Main(){
#if Debug
Print("Debug");
#elif Release
Print("Release");
#endif
#if LOCAL && !OTHER
Print("local");
#endif
#ifdef GLOBAL
Print("global");
#endif
}}
''')
 write(proj/'bolt.conf','format=1\n[language]\ndefines=["LOCAL"]\n[profiles.Debug]\ndebug_info=false\noptimization="aggressive"\n[profiles.Release]\ndebug_info=false\noptimization="none"\n')
 r=run('run',proj);check(r.stdout.endswith('Debug\nlocal\n'),'Debug define follows profile, not optimization')
 r=run('run',proj,'--release');check(r.stdout.endswith('Release\nlocal\n'),'Release define follows profile, not optimization')
 r=run('run',proj,'--define','GLOBAL');check(r.stdout.endswith('Debug\nlocal\nglobal\n'),'CLI defines combine with project defines')
 r=run('run',proj,'--define','GLOBAL');check('cache hit' in r.stderr,'unchanged define set reuses cache')
 r=run('run',proj);check(r.stdout.endswith('Debug\nlocal\n') and 'cache hit' not in r.stderr,'changed CLI define invalidates cache')
 r=run('check',proj,'--define','Windows',ok=False);check(r.returncode!=0 and 'reserved' in r.stderr,'CLI built-in override rejected')
 r=run('check',proj,'--define','A=B',ok=False);check(r.returncode!=0,'CLI text macro syntax rejected')
 r=run('run',proj,'--no-compile','--define','OTHER',ok=False);check(r.returncode!=0,'no-compile refuses changed define option')
 info=json.loads(run('project','info',proj,'--format','json').stdout);check(info['language']['defines']==['LOCAL'],'project model publishes configured defines')
 info=json.loads(run('project','info',proj,'--format','json','--profile','Release','--define','GLOBAL').stdout);check(info['conditional_symbols'].get('Release') and 'Debug' not in info['conditional_symbols'] and info['conditional_symbols'].get('GLOBAL') and info['conditional_symbols'].get('LOCAL'),'project info exposes selected profile and effective CLI symbols')
 check(not info['source_diagnostics']['diagnostics'],'project info analyzes the requested conditional profile')
 write(proj/'src/Independent.bolt','#define OTHER\nclass Independent {}\n')
 check(run('run',proj).stdout.endswith('Debug\nlocal\n'),'source defines never leak into other source files')
 r=run('exec','#ifdef GLOBAL\nPrint(12);\n#endif','--define','GLOBAL');check(r.stdout.endswith('12\n'),'interpreted statement API receives defines')
 backends=run('backends').stdout
 if 'LLVM: ' in backends and 'LLVM: not available:' not in backends:
  r=run('exec','#ifdef Release\nPrint(19);\n#endif','--jit','--release');check(r.stdout.endswith('19\n'),'JIT statement API receives release configuration')
 else:print('SKIP release JIT conditional configuration: compatible LLVM shared library is not installed',flush=True)
 # Different configurations are applied to each local library, not inherited from root.
 lib=root/'Library';write(lib/'bolt.proj','format=1\n[project]\nname="Library"\nkind="static-library"\n')
 write(lib/'bolt.conf','format=1\n[language]\ndefines=["LIBRARY"]\n')
 write(lib/'src/Value.bolt','''namespace Lib;
public static class Value {public:int Get(){
#if LIBRARY && !APP
return 40;
#else
return BadLibraryCondition;
#endif
}
int Extra(){
#ifdef GLOBAL
return 2;
#else
return 1;
#endif
}}
''')
 app=root/'App';write(app/'bolt.proj','format=1\n[project]\nname="App"\n[dependencies]\nLibrary={path="../Library"}\n')
 write(app/'bolt.conf','format=1\n[language]\ndefines=["APP"]\n')
 write(app/'src/Main.bolt','''import Lib
static class Program {public:void Main(){
#if APP && !LIBRARY
Print(Value.Get()+Value.Extra());
#else
UnknownAppCondition();
#endif
}}
''')
 flags=['--no-pch']+(['--cxx',a.cxx] if a.cxx else [])
 r=run('run',app,*flags);check(r.stdout.endswith('41\n'),'dependency source/compiler use same own project defines')
 r=run('run',app,'--define','GLOBAL',*flags);check(r.stdout.endswith('42\n'),'explicit CLI defines propagated to library builds')
 # A disabled attribute or platform declaration must not affect backend choice.
 lone=root/'Inactive.bolt';write(lone,'#if false\n[Native("not_a_type")] class NativeType{}\n#endif\nstatic class Program{public:void Main(){Print(5);}}')
 r=run('run',lone,'--require-native');check(r.stdout.endswith('5\n'),'inactive native declarations do not force compatibility backend')
 write(lone,'#if true\nclass A {}\n#else\n\n#endif\nstatic class Program{public:void Main(){Lol();}}\n')
 r=run('check',lone,ok=False);check(r.returncode!=0 and 'Inactive.bolt:6:' in r.stderr,'semantic diagnostics retain original source lines')
print(f'{count} conditional-compilation CLI groups passed')
