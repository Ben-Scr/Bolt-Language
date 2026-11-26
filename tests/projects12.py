#!/usr/bin/env python3
"""Execute real project/config/library builds. Not an IntelliJ UI/platform test."""
import argparse, json, subprocess, tempfile, pathlib, sys, os
P=pathlib.Path
p=argparse.ArgumentParser();p.add_argument('--bolt',required=True,type=P);p.add_argument('--cxx',default='');a=p.parse_args();B=str(a.bolt.resolve());passed=0

def check(ok,message):
 global passed
 assert ok,message
 passed+=1;print('PASS '+message,flush=True)
def run(*args,cwd=None,ok=True):
 r=subprocess.run([B,*map(str,args)],cwd=cwd,text=True,encoding='utf-8',errors='replace',capture_output=True,timeout=240)
 if ok and r.returncode:raise AssertionError(str(args)+'\n'+r.stdout+r.stderr)
 return r
def write(path,text):path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8')
def manifest(path,name,kind='executable',extra=''):
 write(path/'bolt.proj',f'format = 1\n[project]\nname = {json.dumps(name)}\nkind = "{kind}"\n'+extra)
def src(path,text):write(path/'src/Main.bolt',text)
with tempfile.TemporaryDirectory(prefix='bolt-project12-') as d:
 root=P(d);proj=root/'First ü spaced';run('init',proj)
 check((proj/'src/Main.bolt').exists(),'init creates real minimal source and mandatory manifest')
 check(run('init',proj,ok=False).returncode!=0,'init refuses overwrite')
 info=json.loads(run('project','info',proj,'--format','json').stdout)
 check(info['schema_version']==1 and info['kind']=='executable' and info['default_profile']=='Debug','shared project JSON schema/defaults')
 check(len(info['entrypoints'])==1 and info['entrypoints'][0]['source_hash'],'compiler resolves entry with source identity')
 check(P(json.loads(run('project','info',proj/'src','--format','json').stdout)['root'])==proj,'nearest parent discovery works')
 write(proj/'src/Helper.bolt','public class Helper { public: int Value = 3; }')
 manifest(proj/'src/nested','Nested');src(proj/'src/nested','this is invalid')
 write(proj/'src/bin/Generated.bolt','invalid')
 info=json.loads(run('project','info',proj,'--format','json').stdout)
 check(len(info['sources'])==2,'new source discovery respects nested projects and generated directory boundaries')
 (proj/'src/Helper.bolt').unlink();check(len(json.loads(run('project','info',proj,'--format','json').stdout)['sources'])==1,'removed source invalidates discovery')
 run('check',proj);check(True,'minimal project compiles semantically')
 r=run('run',proj);check('Hello from Bolt!' in r.stdout,'native project run with spaces and Unicode')
 r=run('run',proj);check('cache hit' in r.stderr or 'Up to date' in r.stdout,'unchanged project reuses build cache')
 # force same backend as previous run in case cxx explicitly requested
 src(proj,'INVALID CURRENT CODE');r=run('run',proj,'--no-compile');check('Hello from Bolt!' in r.stdout,'saved build runs despite current source errors')
 r=run('run',proj,ok=False);check(r.returncode!=0 and 'Hello from Bolt!' not in r.stdout,'failed build never starts old executable')
 src(proj,'public static class Program { public: void Main(string[] args) { Print(args.Length); if(args.Length>0) Print(args[0]); } }')
 write(proj/'bolt.conf','format=1\n[run]\narguments=["from config"]\nworking_directory="."\n[profiles.Debug]\noptimization="none"\ndebug_info=false\n')
 r=run('run',proj);check('1\nfrom config\n' in r.stdout,'config program arguments applied')
 r=run('run',proj,'--','cli arg');check('1\ncli arg\n' in r.stdout,'explicit CLI args override config without shell splitting')
 r=run('run',proj,'--');check(r.stdout.endswith('0\n'),'explicit empty argument list clears defaults')
 r=run('compile',proj,'--release');release=json.loads(run('project','info',proj,'--profile','Release','--format','json').stdout)
 check(P(release['artifact']).exists() and '/Release/' in release['artifact'],'separate Release native artifact')
 # Schema errors are not ignored
 old=(proj/'bolt.conf').read_text()
 for conf in ['format=2\n','format=1\n[build]\nwat=true\n','format=1\n[run]\narguments=12\n','format=1\n[language]\nreflection="all"\n','format=1\n[build]\ntarget="moon"\n','format=1\n[run\n']:
  write(proj/'bolt.conf',conf);r=run('project','info',proj,'--format','json',ok=False);check(r.returncode!=0 and 'bolt.conf:' in r.stderr,'positioned schema/TOML rejection '+repr(conf[:35]))
 write(proj/'bolt.conf',old)
 write(proj/'bin/user.txt','must stay');run('clean',proj)
 check((proj/'bin/user.txt').read_text()=='must stay' and (proj/'src/Main.bolt').exists() and not P(release['artifact']).exists(),'clean removes owned artifacts only')
 # Expected .bolt semantics retained; ReadOnly cleanup isn't absence-of-setter enforcement
 src(proj,'import System.Diagnostics\npublic static class Program {public: void Main(){const Stopwatch w=Stopwatch(); Print(w.ElapsedMilliseconds);}}')
 run('check',proj);check(True,'const receiver getter works without redundant ReadOnly')
 src(proj,'import System.Diagnostics\npublic static class Program {public: void Main(){Stopwatch w; w.ElapsedMilliseconds=3;}}')
 check(run('check',proj,ok=False).returncode!=0,'getter-only properties remain non-assignable')
 src(proj,'public class Counter {public: int X; int Next {get { X++; return X; }} } public static class Program{public:void Main(){Counter c;Print(c.Next);Print(c.Next);}}')
 check(run('run',proj).stdout.endswith('1\n2\n'),'getter without setter may mutate receiver; no false const semantics')
 # local libraries built as real distinct native artifacts, no Main needed
 lib=root/'Math';shared=root/'Shared';app=root/'Application'
 manifest(lib,'Math','static-library');src(lib,'namespace GameMath; public static class Arithmetic {public: int Value(){return 7;} T Echo<T>(T v){return v;} }')
 write(lib/'src/Functions.bolt','namespace GameMath; public int Identity(int value) { return value; }')
 manifest(shared,'Shared','shared-library');src(shared,'namespace GameWorld; public static class World {public: int Value(){return 20;} }')
 manifest(app,'Application',extra='[dependencies]\nMath={path="../Math"}\nShared={path="../Shared"}\n');src(app,'import GameMath\nimport GameWorld\npublic static class Program {public:void Main(){Print(GameMath.Identity(Arithmetic.Value())+World.Value());Print(Arithmetic.Echo<int>(3));}}')
 flags=['--no-pch']+(['--cxx',a.cxx] if a.cxx else [])
 r=run('run',app,*flags);check(r.stdout.endswith('27\n3\n'),'real static/shared linking and generic dependency instantiation')
 li=json.loads(run('project','info',lib,'--format','json').stdout);si=json.loads(run('project','info',shared,'--format','json').stdout)
 check(P(li['artifact']).exists() and P(si['artifact']).exists() and not li['entrypoints'],'both library artifacts exist with no Main')
 check(run('run',lib,ok=False).returncode!=0,'library run rejected')
 src(lib,'namespace GameMath; public static class Arithmetic {public: int Value(){return 8;} T Echo<T>(T v){return v;} }')
 check(run('run',app,*flags).stdout.endswith('28\n3\n'),'dependency edits invalidate consumer binary')
 write(lib/'bolt.proj','format=1\n[project]\nname="Math"\nkind="static-library"\n[dependencies]\nApplication={path="../Application"}\n')
 check(run('project','info',app,'--format','json',ok=False).returncode!=0,'dependency cycle rejected')
 manifest(lib,'Math','static-library')
 # no compilation during info: run with broken CXX; still model obtained
 env=os.environ.copy();env['CXX']='/missing/compiler'
 check(subprocess.run([B,'project','info',app,'--format','json'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE).returncode==0,'project info does not require compiler or invoke build')
 src(proj,'public static class Program{public:void Main(){Print(1);}}')
 # compile concurrently to same profile, inspect both statuses
 processes=[subprocess.Popen([B,'compile',str(proj)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) for _ in range(2)]
 outputs=[p.communicate(timeout=150) for p in processes];check(all(p.returncode==0 for p in processes),'parallel same-profile builds serialize cleanly')
 # Preserve explicit single-file execution path
 lone=root/'lone.bolt';write(lone,'public static class Program{public:void Main(){Print(5);}}')
 check(run('run',lone).stdout.endswith('5\n'),'legacy single-file workflow retained')
print(f'{passed} project-system checks passed')
