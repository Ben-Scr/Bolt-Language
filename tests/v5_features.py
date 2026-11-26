#!/usr/bin/env python3
"""Run interface/string/operator cases in the interpreter, including sanitizer builds.
The same corpus is also exercised by native_parity.py and direct_parity.py.
Extra assertions verify original-file diagnostics inside interpolation expressions.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt',type=Path,required=True)
    args=parser.parse_args();bolt=str(args.bolt.resolve());passed=0;failed=0
    def run(*argv):
        return subprocess.run([bolt,*map(str,argv)],input='',capture_output=True,text=True,
                              encoding='utf-8',errors='replace',timeout=30)
    cases=json.loads((ROOT/'tests/v5_cases.json').read_text())+json.loads((ROOT/'tests/arena_cases.json').read_text())
    for case in cases:
        try:
            r=run('run',ROOT/case['file'],'--runtime')
            if case.get('phase')=='compile':
                assert r.returncode!=0 and case['error'] in r.stderr,r.stdout+r.stderr
            else:
                assert r.stdout==case.get('stdout',''),repr(r.stdout)+'\n'+r.stderr
                if case.get('error'):assert r.returncode!=0 and case['error'] in r.stderr,r.stderr
                else:assert r.returncode==case.get('exit',0),r.stderr
            passed+=1;print('PASS '+case['name'],flush=True)
        except (AssertionError,subprocess.TimeoutExpired,OSError) as error:
            failed+=1;print('FAIL '+case['name']+': '+str(error),flush=True)
    with tempfile.TemporaryDirectory(prefix='bolt05-diagnostics-') as directory:
        file=Path(directory)/'Program.bolt'
        # Both successful nested token remapping and lexer-error remapping are checked.
        diagnostics=[
            ('unknown-name','BLT2003','Missing', '        Print($"x {Missing}");'),
            ('nested-unknown','BLT2003','Missing','        Print($"x {$"inner {Missing}"}");'),
            ('hole-lex-error','BLT1001','@','        Print($"x {4 @ 2}");'),
            ('nested-lex-error','BLT1001','@','        Print($"x {$"inner {4 @ 2}"}");'),
        ]
        for name,code,token,line in diagnostics:
            try:
                text='static class Program {\npublic:\n    void Main() {\n'+line+'\n    }\n}\n';file.write_text(text)
                result=run('check',file,'--diagnostics','json')
                records=json.loads(result.stdout)['diagnostics'];assert result.returncode!=0 and records,records
                d=records[0];loc=d['location']
                assert Path(loc['file'])==file and loc['line']==4 and loc['column']==line.index(token)+1,d
                assert d['code']==code and d['sourceLine']==line,d
                passed+=1;print('PASS '+name+' source-position',flush=True)
            except (AssertionError,KeyError,ValueError,subprocess.TimeoutExpired,OSError) as error:
                failed+=1;print('FAIL '+name+': '+str(error),flush=True)
    print(f'{passed} language-feature/diagnostic checks passed, {failed} failed')
    return 1 if failed else 0
if __name__=='__main__':raise SystemExit(main())
