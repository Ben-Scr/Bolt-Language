#!/usr/bin/env python3
"""Execute optionals through each available backend; reject unsafe implicit unwraps."""
import argparse, os, pathlib, shutil, tempfile
from native_parity import execute
P=pathlib.Path
p=argparse.ArgumentParser();p.add_argument('--bolt',required=True,type=P);p.add_argument('--cxx',default='');p.add_argument('--release',action='store_true');p.add_argument('--skip-native',action='store_true');p.add_argument('--use-pch',action='store_true',help='Use the C++ backend precompiled-header cache');a=p.parse_args();B=str(a.bolt.resolve());count=0

cpp_flags=['--backend','cpp']+([] if a.use_pch else ['--no-pch'])+(['--cxx',a.cxx] if a.cxx else [])+(['--release'] if a.release else [])

def check(ok,msg):
 global count
 if not ok:raise AssertionError(msg)
 count+=1;print('PASS '+msg,flush=True)
def run(*args,ok=True,timeout=180):
 r=execute([B,*map(str,args)], stdin=None, timeout=timeout, cwd=None)
 if ok and r.returncode:raise AssertionError(str(args)+'\n'+r.stdout+r.stderr)
 return r

def program(body):return 'public static class Program { public: void Main(){\n'+body+'\n} }\n'
with tempfile.TemporaryDirectory(prefix='bolt-optionals13-') as d:
 root=P(d);source=root/'Optionals.bolt';source.write_bytes(P(__file__).with_name('Optional13.bolt').read_bytes())
 modes=[('Interpreter',['--runtime']),('Fast JIT',['--jit','--backend','fast']),('LLVM JIT',['--jit','--backend','llvm','--release'])]
 if not a.skip_native:
  modes.extend([('Fast AOT',['--backend','fast','--no-debug-info','--require-native']),('LLVM AOT',['--backend','llvm','--release','--require-native']),('C++',cpp_flags)])
 available=[]
 for name,flags in modes:
  r=run('run',source,*flags,ok=False)
  if r.returncode and ('only available on' in r.stderr or 'LLVM shared library not found' in r.stderr):
   print('SKIP '+name+': backend not available',flush=True);continue
  check(r.returncode==0 and 'assertions=55 failures=0' in r.stdout,name+' executes 55 optional assertions\n'+r.stdout+r.stderr if r.returncode else name+' executes 55 optional assertions')
  available.append((name,flags))
 empty=root/'Empty.bolt';empty.write_text(program('int? x=null;Print(x.Value);'))
 for name,flags in available:
  r=run('run',empty,*flags,ok=False)
  check(r.returncode!=0 and 'optional value is empty' in r.stderr and 'Empty.bolt' in r.stderr,name+' reports checked empty Value with source location')
 # Negative cases: declarations and invalidated proofs are not silently accepted.
 types='public class Player {public: int X; void Move(){X++;}}\n'
 cases={
  'unguarded member':types+program('Player? p=null;p.Move();'),
  'unguarded scalar':program('int? n=null;int x=n;'),
  'hasvalue readonly':program('int? n=1;n.HasValue=false;'),
  'const payload':program('const int? n=1;n.Value=2;'),
  'const narrowed payload':types+program('const Player? p=Player();if(p!=null)p.Move();'),
  'non-null ordinary value':program('int x=null;'),
  'non-null reference':program('int& x=null;'),
  'optional reference forbidden':program('int&? x=null;'),
  'void optional forbidden':program('void? x=null;'),
  'arena optional forbidden':program('Arena? x=null;'),
  'local const reference temporary':program('const int?& x=3;'),
  'void nested optional':program('int?? x=null;'),
  'wrong payload type':program('int? n="hello";'),
  'mutable optional ref temporary':'static class Program {public:void F(int?& n){n=null;}void Main(){F(3);}}',
  'reference alias invalidates':'static class Program {public:void Main(){int? n=1;int?& alias=n;alias=null;Print(n+1);}}',
  'pointer alias invalidates':'static class Program {public:void Main(){int? n=1;int?* p=&n;*p=null;Print(n+1);}}',
  'call invalidates':'static class Program {public:void Reset(int?& x){x=null;}void Main(){int? n=1;if(n!=null){Reset(n);Print(n+1);}}}',
  'and side effect invalidates':'static class Program{public:bool Reset(int?& x){x=null;return true;}void Main(){int? n=1;if(n!=null && Reset(n))Print(n+1);}}',
  'loop backedge invalidates':'static class Program{public:void Main(){int? n=1;int i=0;while(i<2){Print(n+1);n=null;i++;}}}',
  'no proof across branch':program('int? n=null;if(Console.ReadLine()=="x")n=1;Print(n+1);'),
  'payload reference escapes':'static class Program{public:int& Bad(){int? n=1;return n.Value;}void Main(){}}',
  'recursive inline optional':'class Recursive { Recursive? next; }\n'+program('Recursive r;'),
 }
 for name,text in cases.items():
  f=root/'Invalid.bolt';f.write_text(text)
  r=run('check',f,ok=False)
  check(r.returncode!=0 and 'Invalid.bolt:' in r.stderr,name+' rejected at source')
 # Mutation behind alias is still checked by Value even when users explicitly unwrap.
 safe=root/'Safe.bolt';safe.write_text('static class Program{public:int Read(const int?& n){return n??4;}void Main(){bool? b=true;if(b)Print(Read(null)+Read(3));int? n=2;if(n.HasValue)Print(n+1);}}')
 for name,flags in available:
  r=run('run',safe,*flags)
  check(r.stdout.endswith('7\n3\n'),name+' const argument temporaries and HasValue narrowing')
 # Range-for remains a native-only legacy feature; test optional narrowing there
 # without pretending the interpreter gained unrelated range-for support.
 ranges=root/'Ranges.bolt';ranges.write_text(program('int[]? sequence=new int[2];sequence.Value[0]=3;sequence.Value[1]=4;int sum=0;if(sequence!=null)for(value : sequence){sum+=value;}Print(sum);'))
 for name,flags in available:
  if name=='Interpreter':continue
  r=run('run',ranges,*flags);check(r.stdout.endswith('7\n'),name+' optional range-for narrowing')
 if not a.skip_native:
  j=root/'Json.bolt' ;j.write_text('''import System.Text.Json
import System.Collections.Generic
[Reflect] public class Entry {public:int X=9;}
[Reflect] public class Record {public:int? Number;string? Name;Entry? Object;int? Property {get;set;}List<int?> Items;}
static class Program {public:int Main(){
 Record r; r.Items.Add(null);r.Items.Add(4);r.Property=17;
 string text=Json.Serialize<Record>(r);Record copy=Json.Deserialize<Record>(text);
 if(copy.Number!=null||copy.Name!=null||copy.Object!=null||copy.Property.Value!=17||copy.Items[0]!=null||copy.Items[1].Value!=4)return 1;
 r.Number=0;r.Name="";r.Object=Entry();
 byte[] bytes=Json.SerializeBinary<Record>(r);copy=Json.DeserializeBinary<Record>(bytes);
 if(copy.Number.Value!=0||copy.Name.Value!=""||copy.Object.Value.X!=9)return 2;
 Print("JSON optionals OK");return 0;
}}''')
  flags=cpp_flags
  r=run('run',j,*flags);check(r.stdout.endswith('JSON optionals OK\n'),'JSON/BJS1 reflected optional fields properties and collection elements')
print(f'{count} optional13 execution/rejection groups passed')
