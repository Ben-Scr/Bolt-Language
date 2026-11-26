#!/usr/bin/env python3
"""Source-to-executable regression coverage for the 0.7 native libraries.
All file operations run in a disposable directory. No third-party packages.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main() -> int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bolt', type=Path, required=True)
    p.add_argument('--cxx', default='cl.exe' if os.name=='nt' else 'c++')
    p.add_argument('--release', action='store_true')
    a=p.parse_args();bolt=str(a.bolt.resolve());passed=0
    def success(name):
        nonlocal passed
        passed+=1;print('PASS '+name, flush=True)
    with tempfile.TemporaryDirectory(prefix='bolt-library-ü-') as d:
        work=Path(d);inc=ROOT/'include';sources=work/'sources';sources.mkdir()
        for source in (ROOT/'tests/library').glob('*.bolt'):
            shutil.copy2(source,sources/source.name)
        def run(command,expected=0,timeout=180):
            result=subprocess.run(list(map(str,command)),cwd=work,capture_output=True,text=True,encoding='utf-8',timeout=timeout)
            if result.returncode!=expected:raise AssertionError(f'{command}\nexit {result.returncode}, expected {expected}\n{result.stdout}\n{result.stderr}')
            return result
        def compile(name,extra=()):
            output=work/(name+('.exe' if os.name=='nt' else ''))
            flags=['--backend','cpp','--cxx',a.cxx,'--include',str(inc)]
            if a.release:flags+=['--release']
            run([bolt,'compile',sources/(name+'.bolt'),'-o',output,*flags,*extra],timeout=300)
            return output
        executable=compile('Features')
        report=run([executable]);assert 'Library assertions passed:' in report.stdout,report.stdout
        print(report.stdout.strip(),flush=True);success('integrated library, lifetime, Unicode, concepts, tasks, JSON and ECS assertions')
        program=compile('Entry');report=run([program,'one','two words','Ä😀'])
        assert report.stdout=='3\none\ntwo words\nÄ😀\n',report.stdout
        success('async Main(string[] args), spaces and Unicode arguments')
        report=run([compile('Terminate')],expected=7);assert report.stdout=='',report.stdout
        success('Terminate exit code and non-returning control flow')
        expected='10\nabc\n0\n6\n6\n14\n31\n0\n5\n3.5\n9\n1\n9\n3\n13\n4\n0\n'
        core=sources/'GenericCore.bolt';out=run([bolt,'run',core,'--runtime']);assert out.stdout==expected,out.stdout
        success('interpreter: structural/multi-type concepts, variadic values, constructor and method defaults')
        out=run([compile('GenericCore')]);assert out.stdout==expected,out.stdout
        success('C++: generic/default/variadic core parity')
        backends=run([bolt,'backends']).stdout
        for backend,available in [('fast','Fast: available' in backends),('llvm','LLVM: not available:' not in backends)]:
            if not available:print('SKIP '+backend+' JIT: backend unavailable',flush=True);continue
            out=run([bolt,'run',core,'--jit','--backend',backend]);assert out.stdout==expected,out.stdout
            success(backend+' JIT: generic/default/variadic core parity')
        negatives={
            'variadic-not-last':'static class Program {public: void F(int... values, int n){} void Main(){}}',
            'required-after-default':'static class Program {public: void F(int n=1,int m){} void Main(){}}',
            'abstract-new':'abstract class Shape {public: abstract int Area();} static class Program {public: void Main(){Shape value;}}',
            'unimplemented-abstract':'abstract class Shape {public: abstract int Area();} class Wrong:Shape{} static class Program {public: void Main(){Wrong value;}}',
            'await-outside-async':'static class Program {public: void Main(){await Task.Delay(1);}}',
            'async-borrow':'static class Program {public: async Task<int> Work(int& n){return n;} void Main(){int n=1;Work(n);}}',
            'concept-result':'concept Same<T>{require(T a,T b){a+b -> T;}} class Odd {public:static int operator +(Odd a,Odd b){return 1;}} static class Program{public:T Add<T>(T a,T b):Same<T>{return a+b;} void Main(){Odd a;Add<Odd>(a,a);}}',
            'concept-private-member':'concept HasX<T>{require(T a){a.X -> int;}} class Hidden {private:int X;} static class Program{public:void F<T>(T a):HasX<T>{} void Main(){Hidden h;F<Hidden>(h);}}',
            'finally-return':'static class Program{public:void Main(){try{}finally{return;}}}',
            'struct-inheritance':'class Parent{} struct Child:Parent{} static class Program{public:void Main(){}}',
            'native-abstract':'import System.IO\nclass UserStream:Stream{} static class Program{public:void Main(){}}',
        }
        for name,text in negatives.items():
            path=work/(name+'.bolt');path.write_text(text,encoding='utf-8')
            result=run([bolt,'check',path,'--diagnostics','json'],expected=1)
            diagnostics=json.loads(result.stdout)['diagnostics'];assert diagnostics and diagnostics[0]['code'].startswith('BLT'),result.stdout
            success('compile-time rejection: '+name)
        result=run([bolt,'run',sources/'Features.bolt','--jit','--backend','fast'],expected=1)
        assert 'C++' in result.stderr,result.stderr
        success('unsupported advanced JIT is rejected explicitly, not interpreted silently')
        # Auto selection is exercised without a --backend or --cxx flag.
        output=work/('automatic'+('.exe' if os.name=='nt' else ''))
        result=run([bolt,'compile',sources/'Entry.bolt','-o',output],timeout=300)
        assert 'C++ native backend' not in result.stderr,result.stderr
        assert run([output,'native']).stdout=='1\nnative\n'
        success('automatic direct native backend selection for async entry points')
        # The native C implementation and C++ header API are genuinely compiled.
        ffi=ROOT/'examples/NativeInterop'
        build=work/'foreign-build';run(['cmake','-S',ffi,'-B',build],timeout=90)
        run(['cmake','--build',build,'--config','Release'],timeout=120)
        libs=list((build/'lib').rglob('example.lib' if os.name=='nt' else 'libexample.a'))
        assert len(libs)==1,libs
        shutil.copy2(ffi/'Program.bolt',sources/'Ffi.bolt');shutil.copy2(ffi/'ffi.h',sources/'ffi.h')
        executable=compile('Ffi',['--header-dir',sources,'--link-lib',libs[0]])
        result=run([executable]);assert result.stdout=='2\n9\nHello from C\n6\nBolt calls C UTF-8 Ä\n7\n25\n',result.stdout
        success('C archive + C++ header bindings, UTF-8, references, arrays and native structs')
    print(f'{passed} library test groups passed',flush=True)
    return 0
if __name__=='__main__':raise SystemExit(main())
