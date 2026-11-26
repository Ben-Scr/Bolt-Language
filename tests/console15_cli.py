#!/usr/bin/env python3
"""Check actual compiler severity records, cache path and explicit run-phase boundary."""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--bolt',required=True,type=Path);a=p.parse_args();b=a.bolt.resolve();n=0
with tempfile.TemporaryDirectory(prefix='bolt-console15-') as tmp:
 root=Path(tmp);f=root/'Main.bolt';f.write_text('static class Program{public:void Main(){Print("warning: user output");}}')
 def run(*options,structured=True):
  env=os.environ.copy();env.pop('BOLT_IDE_CONSOLE',None)
  if structured:env['BOLT_IDE_CONSOLE']='1'
  return subprocess.run([str(b),*options],cwd=root,env=env,capture_output=True,text=True,timeout=150)
 def records(r):
  return [json.loads(s[len('\x1eBOLT1 '):]) for s in r.stderr.split('\n') if s.startswith('\x1eBOLT1 ')]
 def check(value,name,detail=None):
  global n
  assert value,(name,detail);n+=1;print('PASS',name,flush=True)
 r=run('run',str(f));check(r.returncode==0,'native run success',r.stderr)
 data=records(r);check(any(d.get('severity')=='info' and 'compilation' in d.get('message','') for d in data),'timing has explicit information severity',r.stderr)
 check(any(d.get('phase')=='run' for d in data),'phase change precedes program execution',data)
 check('warning: user output' in r.stdout,'program text left intact',r.stdout)
 r=run('compile',str(f));data=records(r);check(r.returncode==0 and not any('phase' in d for d in data),'compile has no application phase',data)
 check(any('cache hit' in d.get('message','') for d in data),'cache timing explicit info',data)
 r=run('run',str(f),'--no-compile');data=records(r);check(r.returncode==0 and any('skipped' in d.get('message','') for d in data),'saved run has information record',r.stderr)
 r=run('run',str(f),'--jit');check(r.returncode==0 and any('JIT' in d.get('message','') for d in records(r)),'JIT timing info',r.stderr)
 r=run('run',str(f),'--runtime');check(r.returncode==0 and any('interpreter' in d.get('message','') for d in records(r)),'interpreter timing info',r.stderr)
 r=run('compile',str(f),structured=False);check(r.returncode==0 and '\x1eBOLT1' not in r.stderr and '[bolt]' in r.stderr,'normal terminal output remains plain',r.stderr)
 f.write_text('[Header("cstdlib"), Native("std::abs")] extern "C++" int Magnitude(int value); static class Program{public:void Main(){Print(Magnitude(-1));}}')
 r=run('run',str(f));check(r.returncode==0,'C++ fallback run success',r.stderr)
 check(any(d.get('severity')=='warning' and 'C++ native backend selected' in d.get('message','') for d in records(r)),'automatic C++ fallback warning not error',r.stderr)
 r=run('run',str(f),'--backend','cpp');check(r.returncode==0 and not any(d.get('severity')=='warning' and 'C++ native backend selected' in d.get('message','') for d in records(r)),'explicit C++ is not a fallback warning')
 f.write_text('static class Program{public:void Main(){int x="wrong";}}')
 r=run('compile',str(f));check(r.returncode!=0 and 'error BLT' in r.stderr,'actual compiler diagnostic remains error',r.stderr)
 check(not any(d.get('phase')=='run' for d in records(r)),'failed build does not signal execution')
print(n,'console groups passed')
