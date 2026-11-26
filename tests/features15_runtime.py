#!/usr/bin/env python3
"""Real language tests: callbacks/events, conversions, access-control and defaults."""
import argparse,subprocess,tempfile,json
from pathlib import Path
P=argparse.ArgumentParser();P.add_argument('--bolt',type=Path,required=True);P.add_argument('--cxx',default='c++');P.add_argument('--cpp-only',action='store_true');P.add_argument('--release',action='store_true');A=P.parse_args();B=A.bolt.resolve();R=Path(__file__).resolve().parent
count=0
modes=[('cpp',['--backend','cpp','--cxx',A.cxx]+(['--release'] if A.release else []))]
if not A.cpp_only:
 listing=subprocess.run([str(B),'backends'],capture_output=True,text=True,timeout=20)
 assert listing.returncode==0,(listing.stdout,listing.stderr)
 native=[]
 if 'Fast: available' in listing.stdout:
  native.extend([('fast-jit',['--jit','--backend','fast','--require-native']),('fast-aot',['--backend','fast','--require-native'])])
 else:print('SKIP Fast backend: unsupported host',flush=True)
 if 'LLVM: not available:' not in listing.stdout and 'LLVM: ' in listing.stdout:
  native.extend([('llvm-jit',['--jit','--backend','llvm','--require-native']),('llvm-aot',['--backend','llvm','--release','--require-native'])])
 else:print('SKIP LLVM backend: compatible shared library is not installed',flush=True)
 modes=[('interpreter',['--runtime'])]+native+modes

def run(source,mode):
 p=subprocess.run([str(B),'run',str(source),*mode,'--no-timings'],capture_output=True,text=True,timeout=150)
 return p
for name,options in modes:
 p=run(R/'features15/Full.bolt',options)
 assert p.returncode==0 and 'FAIL:' not in p.stdout and 'CHECKS=33' in p.stdout,(name,p.returncode,p.stdout,p.stderr)
 count+=1;print('PASS',name,flush=True)
for name,options in modes:
 if name=='interpreter':continue # baseline interpreter does not implement exception handlers
 p=run(R/'features15/AdvancedCore.bolt',options)
 assert p.returncode==0 and 'ADVANCED_OK' in p.stdout,(name,p.returncode,p.stdout,p.stderr)
 count+=1;print('PASS advanced refs/exceptions/default-diamond',name,flush=True)
p=run(R/'features15/Advanced.bolt',['--backend','cpp','--cxx',A.cxx]+(['--release'] if A.release else []))
assert p.returncode==0 and 'ADVANCED_OK' in p.stdout,(p.returncode,p.stdout,p.stderr)
count+=1;print('PASS reflected events cannot leak subscriber storage',flush=True)
negative={
 'event-property':('class P{public:event Action<int> E {get;set;}}','P p;','event'),
 'event-class':('event class P{}','P p;','event'),
 'outside-invoke':('class P{public:event Action<int> E;}', 'P p;p.E.Invoke(1);','declaring type'),
 'outside-call':('class P{public:event Action<int> E;}', 'P p;p.E(1);','declaring type'),
 'derived-invoke':('class P{public:event Action<int> E;} class D:P{public:void Fire(){E.Invoke(1);}}','D d;d.Fire();','declaring type'),
 'event-replace':('class P{public:event Action<int> E;}','P p; p.E=null;','subscribed'),
 'event-read':('class P{public:event Action<int> E;}','P p;Action<int> x=p.E;','event'),
 'event-storage-escape':('class P{public:event Action<int> E;}','P p;p.E.Subscribe(Handler);','events expose'),
 'private-event':('class P{private:event Action<int> E;}','P p;p.E+=Handler;','private'),
 'wrong-handler':('class P{public:event Action<int> E;}','P p;p.E+=Wrong;','overload'),
 'not-action':('class P{public:event int E;}','P p;','Action'),
 'event-initializer':('class P{public:event Action<int> E=null;}','P p;','event'),
 'temporary-handler':('class L{public:void H(int n){}}','Action<int> a=L().H;','temporary'),
 'const-handler':('class L{public:void H(int n){}}','const L l=L();Action<int> a=l.H;','mutable'),
 'explicit-assignment':('class N{public:explicit int(){return 1;}}','N n;int v=n;','assign'),
 'default-explicit-assignment':('class N{public:int(){return 1;}}','N n;int v=n;','assign'),
 'explicit-argument':('class N{public:explicit int(){return 1;}}','N n;Handler(n);','overload'),
 'ambiguous-conversion':('class N{public:implicit int(){return 1;}implicit float(){return 1.0f;}}','N n;double value=n;','ambiguous'),
 'cycle':('class A{public:implicit B(){return B();}}class B{public:implicit A(){return A();}}','A a;','cyclic'),
 'missing-contract':('interface I{explicit string();}class C:I{}','C c;','implement'),
 'weaken-implicit':('interface I{implicit int();}class C:I{public:explicit int(){return 2;}}','C c;','implicit'),
 'default-conflict':('interface A{explicit int(){return 1;}}interface B{explicit int(){return 2;}}class C:A,B{}','C c;','ambiguous'),
 'wrong-conversion-args':('class N{public:implicit int(int value){return value;}}','N n;','parameter'),
 'static-conversion':('class N{public:static implicit int(){return 2;}}','N n;','static'),
 'self-conversion':('class N{public:implicit N(){return N();}}','N n;','conversion'),
 'implicit-writable-borrow':('class N{public:implicit int(){return 3;}}','N n;int& value=n;','reference'),
 'two-step-not-inferred':('class A{public:implicit B(){return B();}}class B{public:implicit int(){return 1;}}','A a;int n=a;','assign'),
}
with tempfile.TemporaryDirectory(prefix='bolt-features15-') as d:
 for name,(declarations,body,message) in negative.items():
  f=Path(d)/(name+'.bolt');f.write_text(declarations+'\nstatic class Program{public: void Handler(int n){} void Wrong(string n){} void Main(){'+body+'}}')
  p=subprocess.run([str(B),'check',str(f)],capture_output=True,text=True,timeout=20)
  assert p.returncode!=0 and message.lower() in p.stderr.lower(),(name,p.returncode,p.stdout,p.stderr,message)
  count+=1;print('PASS rejection',name,flush=True)
print(count,'groups passed')
