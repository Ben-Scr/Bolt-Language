"""Measure an independent set of scripts using the v1 timing and verification helpers."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import random
import shutil
import sys
import tempfile

from cases import CASES, digests

HERE = Path(__file__).resolve().parent
BASE = HERE.parent / 'script-baseline'
sys.path.insert(0, str(BASE))
from run import (DEBUG_FLAGS, RELEASE_FLAGS, CSC_FLAGS, ROOT, c_build, collect, csharp_files,
                 describe, dotnet_details, dotnet10_version_key, execute, orders, run_script, sha256_file)

SEED = 106091


def time_processing(case, name, sdks, cc, rounds, rng, expected):
    # Separate executables keep timing/console output out of the end-to-end run.
    stage = case/'instrumented'
    source_root = stage/'scripts'
    source_root.mkdir(parents=True)
    (stage/'script-baseline').mkdir()
    shutil.copy(BASE/'fixture_io.h', stage/'script-baseline')
    shutil.copy(HERE/'input.h', source_root)
    programs = {}
    for label in [*sdks, *cc]:
        build = source_root/label
        build.mkdir()
        bolt = label in sdks
        filename = 'Program.bolt' if bolt else 'main.c'
        lines = (HERE/name/filename).read_text().splitlines()
        load = next(i for i,s in enumerate(lines) if ('File.ReadAllBytes(' if bolt else '= load_bytes(') in s)
        write = next(i for i,s in enumerate(lines) if ('File.WriteAllBytes(' if bolt else 'write_result(') in s)
        if bolt:
            prefix = ['import System.Diagnostics']
            start = ['Stopwatch watch;', 'watch.Start();']
            stop = ['long ticks = watch.ElapsedTicks;', 'Print(ticks);']
        else:
            prefix = ['#include <windows.h>']
            start = ['LARGE_INTEGER freq, a, b;', 'QueryPerformanceFrequency(&freq);', 'QueryPerformanceCounter(&a);']
            stop = ['QueryPerformanceCounter(&b);', 'printf("%lld\\n", (b.QuadPart-a.QuadPart)*10000000LL/freq.QuadPart);']
        src = build/filename
        src.write_text('\n'.join(prefix+lines[:load+1]+start+lines[load+1:write]+stop+lines[write:])+'\n')
        if bolt:
            execute([sdks[label], 'compile', filename, *RELEASE_FLAGS, '-o', 'program.exe'], build)
        else:
            c_build(cc[label], src, build)
        programs[label] = build/'program.exe'
    def sample(label, _):
        result = case/'result.bin'
        result.unlink(missing_ok=True)
        _, ticks = execute([programs[label]], case)
        if sha256_file(result) != expected:
            raise ValueError(f'Invalid instrumented output for {name}/{label}')
        return int(ticks.strip()) * 100
    return collect(programs, rounds, rng, sample)


def sdk_info(exe):
    files = [exe, *(exe.with_name(n) for n in ('BoltCompiler.dll', 'BoltRuntime.dll', 'BoltRuntime.lib', 'BoltAotRuntime.lib'))]
    return {'version': json.loads(execute([exe, 'version', '--json'], ROOT)[1]),
            'hashes': {p.name: sha256_file(p) for p in files if p.is_file()}}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bolt', type=Path, default=ROOT / 'bin/bolt.exe')
    ap.add_argument('--previous', type=Path, help='Earlier SDK executable for interleaved A/B samples')
    ap.add_argument('--clang', type=Path, default=Path(r'C:\Program Files\LLVM\bin\clang.exe'))
    ap.add_argument('--rounds', type=int, default=24)
    ap.add_argument('--json', type=Path, required=True)
    args = ap.parse_args()
    if args.rounds < 3 or not shutil.which('cl'):
        ap.error('Run from an x64 VS developer shell with at least three rounds')
    sdks = {'bolt': args.bolt.resolve(strict=True)}
    if args.previous:
        sdks['previous'] = args.previous.resolve(strict=True)
    cc = {'msvc': Path(shutil.which('cl')), 'clang': args.clang.resolve(strict=True)}
    dotnet, sdk, root = dotnet_details('dotnet')
    roslyn = root / 'sdk' / sdk / 'Roslyn/bincore'
    pack = max((p for p in (root/'packs/Microsoft.NETCore.App.Ref').glob('10.*')
                if dotnet10_version_key(p.name) and (p/'ref/net10.0').is_dir()),
               key=lambda p: dotnet10_version_key(p.name))
    ref = pack/'ref/net10.0'
    references = sorted(ref.glob('*.dll'))
    csc = [dotnet, 'exec', roslyn/'csc.dll', '-noconfig', '@build.rsp']
    manifest = json.loads((HERE/'manifest.json').read_text())
    rng = random.Random(SEED)
    import os
    tools = [*cc.values(), roslyn/'csc.dll', *[Path(os.environ[key]) for key in
             ('BOLT_LINKER', 'BOLT_LLVM_LIBRARY') if os.environ.get(key)]]
    sources = [p for p in HERE.rglob('*') if p.suffix in ('.c','.cs','.h','.bolt','.py') or p.name=='manifest.json']
    sources += [BASE/'WarmHost.cs', BASE/'fixture_io.h', BASE/'run.py', ROOT/'benchmarks/performance/compare.py']
    report = {'suite':'script-holdout-v1', 'utc':datetime.now(timezone.utc).isoformat(),
              'rounds':args.rounds, 'warmups':4, 'seed':SEED, 'cpu':platform.processor(),
              'os':platform.platform(), 'sdks':{k:sdk_info(v) for k,v in sdks.items()},
              'dotnet_sdk':sdk, 'reference_pack':pack.name,
              'tools':{str(p):sha256_file(p) for p in tools},
              'sources':{p.relative_to(ROOT).as_posix():sha256_file(p) for p in sources},
              'flags':{'bolt_debug':DEBUG_FLAGS, 'bolt_release':RELEASE_FLAGS, 'csc':CSC_FLAGS,
                       'msvc':'/O2 /DNDEBUG /std:c11', 'clang':'-O3 -DNDEBUG -std=c11'}, 'cases':{}}
    with tempfile.TemporaryDirectory(prefix='bolt-holdout-') as temp:
        work = Path(temp)
        host = work/'host'
        host.mkdir()
        shutil.copy(BASE/'WarmHost.cs', host)
        libs = [roslyn/f'Microsoft.CodeAnalysis{s}.dll' for s in ('','.CSharp')]
        for dll in libs:
            shutil.copy(dll, host)
        csharp_files(host, references, 'WarmHost', libs)
        execute(csc, host)
        for name, (generator, before, after) in CASES.items():
            case = work/name
            case.mkdir()
            data, answers = generator(False)
            sample, editor_answers = generator(True)
            if manifest[name] != {'full':digests(data, answers), 'editor':digests(sample, editor_answers)}:
                raise ValueError(f'Fixture changed: {name}')
            hashes = manifest[name]['full']['output_sha256']
            warm_hashes = manifest[name]['editor']['output_sha256']
            if warm_hashes[0] == warm_hashes[1]:
                raise ValueError(f'Edit has no effect: {name}')
            (case/'input.bin').write_bytes(data)
            for f in ('Program.bolt','Program.cs'):
                shutil.copy(HERE/name/f, case)
                if (case/f).read_text().count(before) != 1:
                    raise ValueError(f'Ambiguous edit in {name}/{f}')
            row = report['cases'][name] = {'fixture':manifest[name], 'edit':[before,after]}
            exes = {}
            for label in [*sdks,*cc]:
                build = case/label
                build.mkdir()
                if label in sdks:
                    shutil.copy(case/'Program.bolt', build)
                    execute([sdks[label], 'compile', 'Program.bolt', *RELEASE_FLAGS, '-o', 'program.exe'], build)
                else:
                    c_build(cc[label], HERE/name/'main.c', build)
                exes[label] = build/'program.exe'
            row['release'] = collect(exes, args.rounds, rng, lambda label, _: run_script([exes[label]], case, hashes[0]))
            row['processing'] = time_processing(case, name, sdks, cc, args.rounds, rng, hashes[0])
            def fresh(label, _):
                with tempfile.TemporaryDirectory(dir=case) as path:
                    d = Path(path)
                    if label in sdks:
                        shutil.copy(case/'Program.bolt', d)
                        cmd = [sdks[label], 'compile', 'Program.bolt', *DEBUG_FLAGS, '-o', 'program.exe']
                        app = [d/'program.exe']
                    else:
                        shutil.copy(case/'Program.cs', d)
                        csharp_files(d, references, 'Program')
                        cmd, app = csc, [dotnet, d/'Program.dll']
                    ns, _ = execute(cmd, d)
                    run_script(app, case, hashes[0])
                    return ns
            row['debug_cli'] = collect([*sdks,'roslyn'], args.rounds, rng, fresh)
            (case/'input.bin').write_bytes(sample)
            row['warm_pairs'] = {}
            hosts = list(sdks)
            rng.shuffle(hosts)
            for label in hosts:
                measured = orders(['bolt','roslyn'], args.rounds, rng)
                seq = orders(['bolt','roslyn'], 4, rng) + measured
                requests = [f'{lang} {(i+1)%2}' for i,pair in enumerate(seq) for lang in pair]
                _, out = execute([dotnet, host/'WarmHost.dll', sdks[label].with_name('BoltCompiler.dll'),
                                  ref, before, after, *warm_hashes], case, input='\n'.join(requests)+'\n')
                samples = [round(float(t)*1e6) for t in out.splitlines()]
                if len(samples) != len(requests):
                    raise ValueError(f'Incomplete host samples: {name}/{label}')
                langs = [lang for pair in measured for lang in pair]
                row['warm_pairs'][label] = {'order':measured, 'timings':{
                    k:describe([n for l,n in zip(langs,samples[8:]) if l==k]) for k in ('bolt','roslyn')}}
            print(name, {k:{l:v['median_ms'] for l,v in row[k]['timings'].items()}
                         for k in ('debug_cli','release')}, flush=True)
    if report['sdks'] != {k:sdk_info(v) for k,v in sdks.items()}:
        raise RuntimeError('Compiler files changed during measurement')
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
