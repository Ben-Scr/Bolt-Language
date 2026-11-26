#!/usr/bin/env python3
"""Run real native System APIs, errors, cleanup, workers, child processes and loopback IO."""
from __future__ import annotations
import argparse, os, pathlib, subprocess, tempfile, json, sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--bolt',type=pathlib.Path,required=True);p.add_argument('--cxx',default='cl.exe' if os.name=='nt' else 'c++');p.add_argument('--cpp-only',action='store_true');p.add_argument('--release',action='store_true');p.add_argument('--timeout',type=int,default=120,help='Maximum seconds per compiler or program invocation');args=p.parse_args()
 if args.timeout<=0:p.error('--timeout must be positive')
 bolt=args.bolt.resolve();groups=0
 def call(cmd,cwd,expected=0):
  r=subprocess.run(list(map(str,cmd)),cwd=cwd,text=True,encoding='utf-8',errors='replace',stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=args.timeout)
  if r.returncode!=expected:raise AssertionError(f'{cmd}\nexit {r.returncode} expected {expected}\n{r.stdout}\n{r.stderr}')
  return r
 with tempfile.TemporaryDirectory(prefix='bolt-system14-') as tmp:
  work=pathlib.Path(tmp);backends=call([bolt,'backends'],work).stdout
  configs=[]
  if not args.cpp_only:
   if 'Fast: available' in backends:configs.extend([('fast-jit',['--jit','--backend','fast','--require-native']),('fast-aot',['--backend','fast','--require-native'])])
   if 'LLVM: not available:' not in backends:configs.extend([('llvm-jit',['--jit','--backend','llvm','--require-native']),('llvm-release',['--backend','llvm','--release','--require-native'])])
  configs.append(('cpp',['--backend','cpp','--cxx',args.cxx]+(['--release'] if args.release else [])))
  # New C++ output must use the source implementations even in hosted builds.
  generated=work/'process-network.cpp'
  call([bolt,'emit',ROOT/'tests/native/runtime-free-systems.bolt','-o',generated],work)
  code=generated.read_text(encoding='utf-8')
  for name in ('ProcessHandle','PipeHandle','CapturedProcess','SocketHandle','Resolver','IPAddress','IPEndPoint','EnvironmentOps'):
   assert 'bolt::system::'+name not in code,name
  assert 'bolt::library::Process::Terminate' not in code
  groups+=1
  child=work/('child.exe' if os.name=='nt' else 'child')
  child_flags=['--backend','cpp','--cxx',args.cxx] if args.cpp_only else ['--require-native']
  call([bolt,'compile',ROOT/'tests/systems14/Child.bolt',*child_flags,'-o',child,'--no-timings'],work)
  for fixture in ('native/runtime-free-threading','fixtures/HostedTaskCompletion'):
   call([bolt,'run',ROOT/'tests'/f'{fixture}.bolt','--backend','cpp','--cxx',args.cxx,
         '-o',work/('completion'+('.exe' if os.name=='nt' else ''))],work)
   groups+=1
  for label,flags in configs:
   for case in ('Memory','Exceptions','Threading','Process','Network','Diagnostics','RawTransfer'):
    output=work/(label+'-'+case+('.exe' if os.name=='nt' else ''))
    cmd=[bolt,'run',ROOT/'tests/systems14'/f'{case}.bolt',*flags,'--no-timings']
    if '--jit' not in flags:cmd+=['-o',output]
    if case=='Process':cmd+=['--',child]
    r=call(cmd,work)
    prefix=case.upper()+'14:'
    assert prefix in r.stdout,(label,case,r.stdout,r.stderr)
    if case=='Diagnostics':assert 'TRACE14:real-stderr' in r.stderr,r.stderr
    groups+=1;print('PASS',label,case,':',','.join(x for x in r.stdout.splitlines() if '14:' in x or x.startswith('IPV6:')),flush=True)
  # Caller/FFI worker boundaries and destructor failures run out-of-process.
  fatal=work/'fatal.bolt';fatal.write_text('import System\nclass Bad {public: ~Bad(){throw ArgumentException("dtor-escape-14");}} static class Program {public:void Main(){{Bad bad;}}}',encoding='utf-8')
  for label,flags in configs:
   if '--jit' in flags:continue
   out=work/(label+'-fatal'+('.exe' if os.name=='nt' else ''))
   call([bolt,'compile',fatal,*flags,'-o',out,'--no-timings'],work)
   r=subprocess.run([str(out)],cwd=work,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=args.timeout)
   assert r.returncode!=0 and 'dtor-escape-14' in r.stderr,(r.returncode,r.stderr)
   groups+=1;print('PASS',label,'destructor escape terminates',flush=True)
  # Fail-before-start validation: job callback missing, wrong atomic type.
  for name,source in [('no_run','import System.Threading\nclass Empty{}static class Program{public:void Main(){Thread t=Thread.Start(Empty());}}'),('wrong_atomic','import System.Threading\nstatic class Program{public:void Main(){Atomic<string> a;}}')]:
   f=work/(name+'.bolt');f.write_text(source,encoding='utf-8');r=call([bolt,'check',f],work,expected=1);assert 'error BLT' in r.stderr,r.stderr;groups+=1;print('PASS reject',name,flush=True)
 print(f'{groups} System14 groups passed',flush=True)
if __name__=='__main__':main()
