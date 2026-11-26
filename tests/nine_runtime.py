#!/usr/bin/env python3
"""0.9 syntax, source-library and accessor regression suite; each case is compiled/executed."""
from __future__ import annotations
import argparse,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser();p.add_argument('--bolt',type=Path,required=True);p.add_argument('--cxx',default='cl' if os.name=='nt' else 'c++');a=p.parse_args();bolt=str(a.bolt.resolve());passed=0
 def ok(name):
  nonlocal passed;passed+=1;print('PASS '+name,flush=True)
 with tempfile.TemporaryDirectory(prefix='bolt09-values-') as directory:
  work=Path(directory)
  def run(args,code=0,timeout=180):
   result=subprocess.run([bolt,*map(str,args)],cwd=work,text=True,encoding='utf-8',capture_output=True,timeout=timeout)
   if result.returncode!=code:raise AssertionError(f'{args}: exit {result.returncode}, expected {code}\n{result.stdout}\n{result.stderr}')
   return result
  caps=run(['backends']).stdout
  modes=[('default-run',[]),('cpp',['--backend','cpp','--cxx',a.cxx])]
  if 'Fast: available' in caps:modes += [('fast',['--jit','--backend','fast','--require-native'])]
  if 'LLVM: not available:' not in caps:modes += [('llvm-debug',['--jit','--backend','llvm']),('llvm-release',['--jit','--backend','llvm','--release'])]
  expected={'Dispatch':'12\n28\n','Evaluation':'9:5:1:0:1\n7:7:1:1:1\n7:8:1:1:0\n17:2:1\n4\n','Properties':'1\n3\n0\n2\n4\n5\n7\n9\n3\n1\n3\n8\n4\n12\n6\n7\n3\n','PropertyKinds':'8\nHello\n12\n9\n6\nBolt\n','Calendar':'calendar OK\n'}
  for case,out in expected.items():
   source=work/(case+'.bolt');source.write_text((ROOT/'tests/nine'/source.name).read_text())
   for mode,flags in modes:
    result=run(['run',source,'--no-timings',*flags],timeout=300)
    generated=source.stem+('.exe' if os.name=='nt' else '')+'.bolt.cpp'
    actual='\n'.join(line for line in result.stdout.splitlines() if not line.startswith('Built ') and line!=generated)+'\n'
    assert actual==out,(mode,case,actual,out)
    ok(mode+': '+case)
  source=work/'PropertyReflection.bolt';source.write_text((ROOT/'tests/nine/PropertyReflection.bolt').read_text())
  for mode,flags in modes:
   result=run(['run',source,*flags,'--no-timings'],timeout=300)
   assert result.stdout.endswith('7\nBolt\n12\n24\n12\n21\n0\n5\ntrue\ntrue\n0\n'),result.stdout
   ok(mode+': property reflection and JSON get/set; no backing fields or synthetic methods exposed')
  source=work/'AbstractProperty.bolt';source.write_text((ROOT/'tests/nine/AbstractProperty.bolt').read_text())
  result=run(['run',source,'--backend','cpp','--cxx',a.cxx,'--no-timings'],timeout=300)
  assert result.stdout.endswith('12\n'),result.stdout
  ok('abstract property implementation dispatched through base reference')
  negatives={
   'getter-only':'class C { public: int X {get;} } static class Program {public:void Main(){C c;c.X=2;}}',
   'private-set':'class C { public: int X {get;private set;} } static class Program {public:void Main(){C c;c.X=2;}}',
   'write-only-read':'class C {private:int x; public:int X {set{x=value;}}} static class Program{public:void Main(){C c;Print(c.X);}}',
   'const-receiver':'class C {public:int X {get;set;} } static class Program{public:void Main(){const C c; c.X=2;}}',
   'property-is-not-ref':'class C {public:int X {get;set;}} static class Program{public:void F(int& n){n++;} void Main(){C c;F(c.X);}}',
   'temporary-property-set':'class C {public:int X {get;set;}} static class Program{public:void Main(){C().X=2;}}',
   'comma-duplicate':'static class Program{public:void Main(){int x=1,x=2;}}',
   'comma-type':'static class Program{public:void Main(){int x=1,y="wrong";}}',
   'field-duplicate':'class C {public:float X,X;} static class Program{public:void Main(){}}',
   'field-initializer-name':'class C {public:int A; C(int a):Missing(a){}} static class Program{public:void Main(){C c=C(3);}}',
   'field-initializer-duplicate':'class C {public:int A; C(int a):A(a),A(a){}} static class Program{public:void Main(){C c=C(3);}}',
   'accessor-wrong-type':'class C {public:int X {get{return "no";}}} static class Program{public:void Main(){C c;Print(c.X);}}',
   'readonly-side-effect':'class C {private:int x; public:[ReadOnly] int X {get{x++;return x;}}} static class Program{public:void Main(){C c;Print(c.X);}}',
  }
  for name,text in negatives.items():
   source=work/(name+'.bolt');source.write_text(text)
   result=run(['check',source,'--diagnostics','json'],code=1)
   records=json.loads(result.stdout)['diagnostics'];assert records and records[0]['code'].startswith('BLT'),result.stdout
   ok('reject '+name)
  # Standard library implementations can also be parsed as ordinary Bolt sources by tooling;
  # no public Collection/Random/Time/File source class may be a Native type mapping.
  for rel in ('System/Collections/Generic/List.bolt','System/Collections/Generic/HashSet.bolt','System/Collections/Generic/Dictionary.bolt','System/Collections/Generic/Queue.bolt','System/Random.bolt','System/Time.bolt','System/Stopwatch.bolt','System/IO/File.bolt'):
   text=(ROOT/'stdlib'/rel).read_text();assert '[Native(' not in text,rel
   ok('real source library '+rel)
 print(f'{passed} Bolt 0.9 checks passed',flush=True)
 return 0
if __name__=='__main__':raise SystemExit(main())
