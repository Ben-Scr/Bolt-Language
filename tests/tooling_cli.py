#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

def main():
    p=argparse.ArgumentParser();p.add_argument('--bolt',type=Path,required=True);p.add_argument('--cxx',default='');p.add_argument('--skip-native',action='store_true');a=p.parse_args()
    bolt=str(a.bolt.resolve());count=0
    def passed(name):
        nonlocal count
        count+=1;print('PASS '+name)
    def run(*args):return subprocess.run([bolt,*map(str,args)],capture_output=True,text=True,encoding='utf-8',timeout=120)
    with tempfile.TemporaryDirectory(prefix='bolt-diagnostics-') as d:
        root=Path(d)
        for args in [('help',),('help','compile'),('compile','--help'),('--help',),('help','lsp')]:
            r=run(*args);assert r.returncode==0 and r.stdout
        passed('help entry points')
        version=json.loads(run('version','--json').stdout)
        assert run('version').stdout.startswith('Bolt '+version['version']+' ')
        assert Path(version['executable']).samefile(bolt)
        assert version['target'] and version['runtimeAbi']>0 and version['compilerHostAbi']==1
        assert run('version','--unknown').returncode!=0
        passed('version alias')
        project=root/'My Project';r=run('new',project);assert r.returncode==0,r.stderr
        assert (project/'bolt.project').is_file() and (project/'Program.bolt').is_file()
        before=(project/'Program.bolt').read_bytes();assert run('new',project).returncode!=0;assert before==(project/'Program.bolt').read_bytes()
        passed('new creates a project and refuses overwrite')
        r=run('check',project,'--diagnostics','json');assert r.returncode==0 and json.loads(r.stdout)['diagnostics']==[]
        passed('machine-readable successful check')
        first=project/'Program.bolt';second=project/'Bad.bolt'
        first.write_text('static class Program { public: void Main() { int x = ; } }')
        second.write_text('public class Bad { public: int y = ; }')
        r=run('check',project,'--diagnostics','json');ds=json.loads(r.stdout)['diagnostics'];assert r.returncode==1 and len(ds)==2,ds
        passed('one parse diagnostic per broken file, collected across a project')
        second.unlink()
        code='static class Program {\npublic:\n    void Main() {\n        int value = "hello";\n    }\n}\n'
        first.write_text(code)
        r=run('check',first,'--diagnostics','json');result=json.loads(r.stdout);di=result['diagnostics'][0]
        assert di['code']=='BLT2002' and 'string' in di['message'] and 'int' in di['message'],di
        assert di['location']['line']==4 and di['location']['column']==21 and di['location']['length']==7,di
        assert di['sourceLine']=='        int value = "hello";'
        passed('type mismatch has code, actual types and exact initializer range')
        r=run('check',first);assert '^~~~~~~' in r.stderr and 'help:' in r.stderr
        source_line=next(l for l in r.stderr.splitlines() if 'int value =' in l)
        caret_line=next(l for l in r.stderr.splitlines() if '^' in l)
        assert source_line.index('"hello"')==caret_line.index('^'),r.stderr
        passed('human source excerpt and caret alignment')
        r=run('check',first,'--diagnostics','msbuild');assert '(4,21): error BLT2002:' in r.stderr
        passed('MSBuild-compatible diagnostic output format')
        first.write_text('public class Library { public: int X; }')
        assert run('check',first,'--no-entry').returncode==0
        assert run('check',first).returncode!=0
        passed('library checking does not require Main when requested')
        first.write_bytes(b'\xef\xbb\xbf'+code.encode())
        r=run('check',first,'--diagnostics','json');assert json.loads(r.stdout)['diagnostics'][0]['location']['line']==4
        passed('UTF-8 BOM source accepted')
        runtime='static class Program {\npublic:\n    void Main() {\n        int zero = 0;\n        Console.WriteLine(10 / zero);\n    }\n}\n'
        first.write_text(runtime)
        r=run('run',first,'--runtime');assert r.returncode==1 and ':5:30: error BLT3001' in r.stderr,r.stderr
        assert 'at Program.Main' in r.stderr
        passed('interpreter runtime failure points to the operator and method frame')
        if not a.skip_native:
            command=['compile',project,'--debug','--no-pch']
            if a.cxx:command+=['--cxx',a.cxx]
            r=run(*command);assert r.returncode==0,r.stderr
            exe=project/'bin/Debug'/('My Project.exe' if __import__('os').name=='nt' else 'My Project')
            native=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            assert native.returncode==1 and ':5:30: error BLT3001' in native.stderr,native.stderr
            passed('native checked runtime error has the same Bolt source location')
            unrelated=project/'bin/keep.txt';unrelated.write_text('user data')
            r=run('clean',project);assert r.returncode==0,r.stderr
            assert not exe.exists() and unrelated.read_text()=='user data' and first.read_text()==runtime
            passed('clean removes only known build products')
        protected=project/'bin/User.bolt';protected.parent.mkdir(exist_ok=True);protected.write_text('do not delete')
        assert run('clean',project).returncode!=0 and protected.read_text()=='do not delete'
        passed('clean refuses output directories containing Bolt source')
        assert run('help','nonexistent').returncode!=0
        passed('unknown help topic is an error')
    print(f'{count} tooling CLI checks passed')
if __name__=='__main__':main()
