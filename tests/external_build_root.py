#!/usr/bin/env python3
"""Exercise read-only sources, external build records, and verified runtime reuse."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', required=True, type=Path)
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    bolt = args.bolt.resolve()
    with tempfile.TemporaryDirectory(prefix='bolt-external-root-') as temp:
        work = Path(temp)
        external = work / 'Builds ü'
        source = work / 'checkout'
        loose = source / 'main.bolt'
        project = source / 'Project'
        dep = source / 'Dependency'
        for folder in (project, dep):
            (folder / 'src').mkdir(parents=True)
        loose.write_text('static class Program {public: void Main(){Print(37);}}')
        (dep / 'bolt.proj').write_text('format=1\n[project]\nname="Arithmetic"\nkind="static-library"\n')
        (dep / 'src/Lib.bolt').write_text('namespace Arithmetic; public static class Example { public: int Value(){return 91;} }')
        (project / 'bolt.proj').write_text('format=1\n[project]\nname="Consumer"\n[dependencies]\nArithmetic={path="../Dependency"}\n')
        (project / 'src/Main.bolt').write_text('import Arithmetic\nstatic class Program {public: void Main(){Print(Example.Value());}}')
        children = sorted(source.rglob('*'))
        def run(*argv, relocated=True, success=True):
            env = os.environ.copy()
            env.pop('BOLT_BUILD_ROOT', None)
            if relocated:
                env['BOLT_BUILD_ROOT'] = str(external)
            result = subprocess.run([str(bolt), *map(str, argv)], env=env, cwd=work,
                                    capture_output=True, text=True, timeout=240)
            if (result.returncode == 0) != success:
                raise AssertionError(f'{argv}\n{result.stdout}{result.stderr}')
            return result
        lock_permissions = os.name == 'posix' and os.geteuid() != 0
        try:
            if lock_permissions:
                for path in [source, *children]:
                    path.chmod(0o555 if path.is_dir() else 0o444)
                # Earlier versions published an executable and then failed writing .bolt.
                refused = work / 'must stay absent'
                run('compile', loose, '--debug', '--require-native', '-o', refused,
                    relocated=False, success=False)
                assert not refused.exists()
                print('PASS read-only build failure leaves the destination unpublished')
            caps = run('backends').stdout
            profiles = ['--debug']
            if 'LLVM: not available' not in caps:
                profiles.append('--release')
            for profile in profiles:
                out = run('run', loose, profile, '--require-native')
                assert out.stdout.endswith('37\n'), out
                out = run('run', loose, profile, '--require-native')
                assert 'Up to date' in out.stdout and out.stdout.endswith('37\n'), out
                assert run('run', loose, profile, '--no-compile').stdout == '37\n'
            print(f'PASS relocated native builds, cache hits and saved execution ({", ".join(profiles)})')

            flags = ['--backend', 'cpp', '--cxx', args.cxx, '--no-pch']
            out = run('run', project, *flags)
            assert out.stdout.endswith('91\n'), out
            assert run('run', project, '--no-compile').stdout == '91\n'
            infos = [json.loads(run('project', 'info', folder, '--format', 'json').stdout) for folder in (project, dep)]
            for folder, info in zip((project, dep), infos):
                assert Path(info['root']) == folder
                artifact = Path(info['artifact'])
                assert artifact.is_file() and external in artifact.parents, info
                assert external in Path(info['intermediate_directory']).parents
            run('clean', project)
            assert not Path(infos[0]['artifact']).exists() and Path(infos[1]['artifact']).is_file()
            assert sorted(source.rglob('*')) == children
            print('PASS separate dependency outputs, project info and scoped clean on a read-only checkout')
        finally:
            if lock_permissions:
                for path in [source, *children]:
                    path.chmod(0o755 if path.is_dir() else 0o644)

        # On shared-runtime builds, preserve a verified adjacent copy and repair
        # a same-size damaged copy regardless of modification time.
        output = work / ('app.exe' if os.name == 'nt' else 'app')
        debug = ['compile', loose, '--debug', '--require-native', '-o', output]
        run(*debug)
        companions = [file for file in work.iterdir() if file.name in ('libBoltRuntime.so','libBoltRuntime.dylib','BoltRuntime.dll')]
        if companions:
            runtime, = companions
            original = hashlib.sha256(runtime.read_bytes()).digest()
            prior = runtime.stat()
            loose.write_text('static class Program {public: void Main(){Print(38);}}')
            run(*debug)
            assert runtime.stat().st_mtime_ns == prior.st_mtime_ns
            with runtime.open('r+b') as stream:
                stream.seek(-1, os.SEEK_END)
                last = stream.read(1)[0]
                stream.seek(-1, os.SEEK_END)
                stream.write(bytes([last ^ 1]))
            os.utime(runtime, ns=(prior.st_atime_ns, prior.st_mtime_ns))
            failed = run('run', loose, '-o', output, '--no-compile', success=False)
            assert 'runtime file' in failed.stderr, failed
            run(*debug)
            assert hashlib.sha256(runtime.read_bytes()).digest() == original
            assert run('run', loose, '-o', output, '--no-compile').stdout == '38\n'
            print('PASS unchanged shared runtime preserved and same-size, same-timestamp damage detected and repaired')


if __name__ == '__main__':
    main()
