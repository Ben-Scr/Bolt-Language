#!/usr/bin/env python3
"""Run the existing Bolt corpus through direct AOT and JIT, never C++ emission.
LLVM checks are skipped explicitly when a compatible shared LLVM is not installed.
Every process has a timeout. Output/status/error assertions are shared across modes.
"""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt',type=Path,required=True)
    parser.add_argument('--work',type=Path,required=True)
    parser.add_argument('--jobs',type=int,default=2)
    parser.add_argument('--case', action='append', dest='case_names', metavar='NAME',
                        help='Run a named regression case, repeatable')
    parser.add_argument('--runner',type=Path,help='Executable wrapper for running generated AOT programs on another architecture')
    parser.add_argument('--timeout',type=int,default=90,help='Per-process time limit in seconds')
    parser.add_argument('--discard-artifacts',action='store_true',help='Remove each isolated AOT build after its case finishes')
    parser.add_argument('--modes',nargs='+',choices=['fast-jit','fast-aot','llvm-jit','llvm-debug-aot','llvm-release-jit','llvm-release-aot'],
                        help='Select execution modes. llvm-debug-aot is an additional explicit profile.')
    args=parser.parse_args();bolt=str(args.bolt.resolve());work=args.work.resolve();work.mkdir(parents=True,exist_ok=True)
    if args.timeout <= 0:parser.error('--timeout must be positive')
    runner=[str(args.runner.resolve())] if args.runner else []
    def run(command: list[str], stdin: str='') -> subprocess.CompletedProcess[str]:
        return subprocess.run(command,input=stdin,text=True,encoding='utf-8',errors='replace',capture_output=True,cwd=ROOT,timeout=args.timeout)
    listing=run([bolt,'backends']).stdout
    modes=[]
    if 'Fast: available' in listing:modes += [('fast-jit','fast',False,True),('fast-aot','fast',False,False)]
    else: print('SKIP fast backend: unsupported host')
    if 'LLVM: not available:' not in listing and 'LLVM: ' in listing:
        modes += [('llvm-jit','llvm',False,True),('llvm-release-jit','llvm',True,True),('llvm-release-aot','llvm',True,False)]
        if args.modes and 'llvm-debug-aot' in args.modes:
            modes.append(('llvm-debug-aot','llvm',False,False))
    else: print('SKIP LLVM backend: shared LLVM not available')
    if args.modes:
        unavailable=set(args.modes)-{mode[0] for mode in modes}
        if unavailable:parser.error('Requested execution modes are unavailable: '+', '.join(sorted(unavailable)))
        modes=[mode for mode in modes if mode[0] in args.modes]
    cases=[]
    for name in ['parity_cases.json','feature_cases.json','direct_cases.json','v5_cases.json','arena_cases.json']:
        cases += json.loads((ROOT/'tests'/name).read_text(encoding='utf-8'))
    if args.case_names:
        selected = set(args.case_names)
        unknown = selected - {case['name'] for case in cases}
        if unknown:
            parser.error('Unknown corpus case: ' + ', '.join(sorted(unknown)))
        cases = [case for case in cases if case['name'] in selected]
    def check(item):
        mode,case=item;name,backend,release,jit=mode;label=name+'/'+case['name']
        temporary=None
        try:
            source=str(ROOT/case['file']);flags=['--backend',backend]+(['--release'] if release else [])
            if jit:
                result=run([bolt,'run',source,'--jit',*flags],case.get('stdin',''))
            else:
                if args.discard_artifacts:
                    temporary=tempfile.TemporaryDirectory(prefix=name+'-',dir=work)
                    folder=Path(temporary.name)
                else:
                    folder=work/name/case['name'];folder.mkdir(parents=True,exist_ok=True)
                target=folder/('test native'+('.exe' if os.name=='nt' else ''))
                result=run([bolt,'compile',source,'-o',str(target),*flags])
                if case.get('phase')!='compile':
                    if result.returncode:raise AssertionError('compilation failed: '+result.stdout+result.stderr)
                    if Path(str(target)+'.bolt.cpp').exists():raise AssertionError('unexpected C++ intermediate in a direct build')
                    result=run([*runner,str(target)],case.get('stdin',''))
            if case.get('phase')=='compile':
                if result.returncode==0 or case['error'] not in result.stderr:raise AssertionError('expected compile diagnostic: '+result.stderr)
            else:
                if result.stdout!=case.get('stdout',''):raise AssertionError('stdout: '+repr(result.stdout)+' expected '+repr(case.get('stdout',''))+'\n'+result.stderr)
                if case.get('error'):
                    if result.returncode==0 or case['error'] not in result.stderr:raise AssertionError('expected error: '+result.stderr)
                elif result.returncode!=case.get('exit',0):raise AssertionError('unexpected exit: '+str(result.returncode)+'\n'+result.stderr)
            return True,'PASS '+label
        except (OSError,AssertionError,subprocess.TimeoutExpired) as error:return False,'FAIL '+label+': '+str(error)
        finally:
            if temporary is not None:temporary.cleanup()
    items=[(mode,case) for mode in modes for case in cases]
    failed=0
    with ThreadPoolExecutor(max_workers=max(1,args.jobs)) as workers:
        for ok,message in workers.map(check,items):
            print(message,file=sys.stdout if ok else sys.stderr,flush=True)
            if not ok:failed+=1
    print(f'{len(items)-failed} direct-backend checks passed, {failed} failed ({len(cases)} cases x {len(modes)} execution modes)')
    return 1 if failed else 0
if __name__=='__main__':raise SystemExit(main())
