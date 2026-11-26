#!/usr/bin/env python3
"""Execute source-library migration tests and compare BJS1 with an independent codec."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import random
import platform
import sys
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / 'tests/migration11'

def binary(value):
    if value is None: return b'\x00'
    if value is False: return b'\x01'
    if value is True: return b'\x02'
    if isinstance(value, int):
        return (b'\x03'+struct.pack('<q', value)) if value < 0 else (b'\x04'+struct.pack('<Q', value))
    if isinstance(value, float): return b'\x05'+struct.pack('<d', value)
    if isinstance(value, str):
        text=value.encode('utf-8');return b'\x06'+struct.pack('<Q',len(text))+text
    if isinstance(value, list): return b'\x07'+struct.pack('<Q',len(value))+b''.join(binary(x) for x in value)
    result=b'\x08'+struct.pack('<Q',len(value))
    for key in sorted(value):
        text=key.encode('utf-8');result+=struct.pack('<Q',len(text))+text+binary(value[key])
    return result

def json_corpus():
    rng=random.Random(20260929)
    atoms=[None,True,False,0,-1,-9223372036854775808,18446744073709551615,1.25,-0.0,'','Ä😀','\"\\\n\t\x00','/']
    def value(depth=0):
        choice=rng.randrange(5)
        if depth>=4 or choice<3:return rng.choice(atoms)
        if choice==3:return [value(depth+1) for _ in range(rng.randrange(6))]
        return {f'key{i}😀':value(depth+1) for i in range(rng.randrange(6))}
    corpus=atoms+[[i for i in range(500)],{'b':'second','a':'first'}]+[value() for _ in range(60)]
    # Exercise normal and subnormal binary64 boundaries and both tie directions.
    corpus += [sys.float_info.max, sys.float_info.min, 5e-324, 1e-323, 0.1,
               -1e-300, 1.0000000000000002, 3.4028234663852886e38]
    for _ in range(48):
        bits = rng.randrange(0x7ff0000000000000) | (rng.getrandbits(1) << 63)
        corpus.append(struct.unpack('<d', struct.pack('<Q', bits))[0])
    return corpus

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    parser.add_argument('--cxx', default='cl.exe' if os.name=='nt' else 'c++')
    parser.add_argument('--release', action='store_true', help='Optimize the generated C++ programs.')
    args=parser.parse_args();bolt=str(args.bolt.resolve());groups=0
    def passed(name):
        nonlocal groups
        groups+=1; print('PASS '+name,flush=True)
    with tempfile.TemporaryDirectory(prefix='bolt-source11-') as directory:
        work=Path(directory)
        def run(command,expected=0,timeout=300):
            result=subprocess.run(list(map(str,command)),cwd=work,text=True,encoding='utf-8',capture_output=True,timeout=timeout)
            if result.returncode != expected:
                raise AssertionError(f'{command}\nexit={result.returncode}, expected={expected}\n{result.stdout}\n{result.stderr}')
            return result
        def fixture():
            outside=work/'outside';outside.mkdir(exist_ok=True);(outside/'keep.txt').write_text('keep',encoding='utf-8')
            links=work/'links';links.mkdir(exist_ok=True)
            try: (links/'foreign').symlink_to(outside,target_is_directory=True)
            except (OSError,NotImplementedError):print('NOTE symlink creation unavailable; traversal test has an empty directory.',flush=True)
        backends=run([bolt,'backends']).stdout
        configs=[]
        if 'Fast: available' in backends:configs += [('fast-jit',['--jit','--backend','fast']),('fast-aot',['--backend','fast'])]
        if 'LLVM: not available:' not in backends:configs += [('llvm-jit',['--jit','--backend','llvm']),('llvm-release-aot',['--backend','llvm','--release'])]
        for label,flags in configs:
            fixture()
            if '--jit' not in flags:flags=[*flags,'-o',str(work/label)]
            result=run([bolt,'run',SOURCES/'Core.bolt',*flags,'--require-native','--no-timings'])
            assert 'Core11 checks:' in result.stdout and '\x1b' not in result.stdout,result.stdout
            assert (work/'outside/keep.txt').read_text()=='keep'
            print(result.stdout.strip(),flush=True);passed('direct '+label+': Console/Thread/Stopwatch/Converter/File/Directory/StreamWriter')
            result=run([bolt,'run',SOURCES/'Async.bolt',*flags,'--require-native','--no-timings'])
            assert 'Async11 checks:' in result.stdout,result.stdout
            passed(label+' Task result sharing, faults, WhenAll and receiver snapshots')
        cppflags=['-o',str(work/'cpp-program'),'--backend','cpp','--cxx',args.cxx,'--no-timings']+(['--release'] if args.release else [])
        fixture();r=run([bolt,'run',SOURCES/'Core.bolt',*cppflags]);assert 'Core11 checks:' in r.stdout,r.stdout
        print(r.stdout.strip(),flush=True);passed('C++ parity for migrated synchronous APIs')
        r=run([bolt,'run',SOURCES/'Async.bolt',*cppflags]);assert 'Async11 checks:' in r.stdout,r.stdout
        print(r.stdout.strip(),flush=True);passed('Task source facade, void await, WhenAll faults and source file async helpers')
        corpus=json_corpus()
        (work/'corpus.jsonl').write_text(''.join(json.dumps(v,ensure_ascii=False,separators=(',',':'))+'\n' for v in corpus),encoding='utf-8')
        json_modes=[('C++',cppflags)]
        for label,flags in configs:
            options=[*flags,'--require-native','--no-timings']
            options+=['--steps','0'] if '--jit' in flags else ['-o',str(work/(label+'-json'))]
            json_modes.append((label,options))
        if (os.name == 'nt' or platform.system() == 'Linux') and 'LLVM: not available:' not in backends:
            json_modes.append(('runtime-free LLVM', ['--release','--no-runtime','--no-timings','-o',str(work/'standalone-json')]))
        for label,flags in json_modes:
            r=run([bolt,'run',SOURCES/'Json.bolt',*flags]);assert 'Json11 checks:' in r.stdout,r.stdout
            lines=r.stdout.splitlines();texts=[x[5:] for x in lines if x.startswith('TEXT:')];bytesets=[x[6:] for x in lines if x.startswith('BYTES:')];backs=[x[5:] for x in lines if x.startswith('BACK:')]
            assert len(texts)==len(corpus)==len(bytesets)==len(backs),r.stdout
            for original,text,encoded,back in zip(corpus,texts,bytesets,backs):
                assert json.loads(text)==original,(original,text)
                assert text==back,(text,back)
                actual=bytes(json.loads(encoded));expected=b'BJS\x01'+binary(original)
                assert actual==expected,(original,actual,expected)
            print(next(line for line in lines if line.startswith('Json11 checks:')),flush=True)
            passed(f'{label} JSON text + BJS1, {len(corpus)} independent differential values, typed/reflected roundtrips and malformed inputs')
        # Every prefix of a representative valid binary must fail as a truncated input.
        negative=work/'negative.bolt'
        negative.write_text('''import System\nimport System.Text.Json\npublic static class Program {public:void Main(){
        byte[] valid = Json.SerializeBinary<string>("Ä😀 text");
        for(int n=0;n<valid.Length;n++){byte[] part=new byte[n];for(int i=0;i<n;i++)part[i]=valid[i];bool failed=false;
        try{Json.DeserializeBinary<string>(part);}catch(JsonException& error){failed=true;}
        if(!failed)throw Exception("Accepted a truncated binary");}Print("truncated binaries rejected");}}''',encoding='utf-8')
        r=run([bolt,'run',negative,*cppflags]);assert 'truncated binaries rejected' in r.stdout,r.stdout
        passed('all truncated prefixes rejected with JsonException')
        # Invalid conversion categories stay distinguishable; TryTo is non-throwing.
        negative.write_text('''import System\npublic static class Program{public:void Main(){
        int passed=0;try{Converter.ToInt("2147483648");}catch(OverflowException& e){passed++;}
        try{Converter.ToUInt64("-1");}catch(OverflowException& e){passed++;}
        try{Converter.ToInt("99999999999999999x");}catch(FormatException& e){passed++;}
        try{Converter.ToBool("yes");}catch(FormatException& e){passed++;}
        try{Converter.ToChar("");}catch(FormatException& e){passed++;}
        if(passed!=5)throw Exception("wrong conversion exception type");Print("conversion errors checked");}}''',encoding='utf-8')
        r=run([bolt,'run',negative,*cppflags]);assert 'conversion errors checked' in r.stdout,r.stdout
        passed('format vs overflow behavior preserved')
    print(f'{groups} source-migration integration groups passed',flush=True)
    return 0
if __name__=='__main__':raise SystemExit(main())
