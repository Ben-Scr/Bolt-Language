#!/usr/bin/env python3
"""Exercise Linux x64/AArch64/RV64 cross objects and, with a target SDK, run linked programs."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile


def run(*args, error=None, cwd=None):
    result = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True, text=True, timeout=240)
    output = result.stdout + result.stderr
    if error:
        if result.returncode == 0 or error not in output:
            raise AssertionError(f'{args}: expected {error!r}, returned {result.returncode}\n{output}')
    elif result.returncode:
        raise AssertionError(f'{args}: exit {result.returncode}\n{output}')
    return output


def elf(path, architecture, kinds):
    with path.open('rb') as stream:
        header = stream.read(64)
    kind, machine = struct.unpack_from('<HH', header, 16)
    if header[:7] != b'\x7fELF\x02\x01\x01' or kind not in kinds or machine != architecture:
        raise AssertionError(f'Unexpected ELF kind {kind} / machine {machine} in {path}')
    if architecture == 243 and struct.unpack_from('<I', header, 48)[0] & 6 != 4:
        raise AssertionError(f'RV64 requires the LP64D hard-float ABI in {path}')


def exercise_dependencies(bolt, work, target, machine, runner, flags):
    # Move the executable and DSOs away and remove their original build paths
    # before launching. This detects absolute DT_NEEDED and broken $ORIGIN.
    root = work / 'library graph'
    math, shared, app = (root / name for name in ('Math', 'Shared', 'Application'))
    fixtures = [
        (math, 'static-library', '', 'namespace GameMath; public static class MathLib {public: int Value(){return 22;}}'),
        (shared, 'shared-library', '', 'namespace GameWorld; public static class World {public: int Value(){return 20;}}'),
        (app, 'executable', '[dependencies]\nMath={path="../Math"}\nShared={path="../Shared"}\n',
         'import GameMath\nimport GameWorld\npublic static class Program {public: void Main(){Print(MathLib.Value()+World.Value());}}'),
    ]
    for path, kind, deps, source in fixtures:
        (path / 'src').mkdir(parents=True)
        (path / 'bolt.proj').write_text(f'format=1\n[project]\nname="{path.name}"\nkind="{kind}"\n' + deps)
        (path / 'src/Main.bolt').write_text(source)
    run(bolt, 'compile', app, '--debug', '--no-debug-info', '--target', target, *flags)
    output = app / 'bin' / target / 'Debug'
    deployed = output / 'libShared.so'
    executable = output / app.name
    elf(deployed, machine, (3,))
    elf(executable, machine, (2, 3))
    bundle = work / 'moved application'
    bundle.mkdir()
    shutil.copy2(deployed, bundle / deployed.name)
    runtime = output / 'libBoltRuntime.so'
    if runtime.is_file():
        shutil.copy2(runtime, bundle / runtime.name)
    shutil.copy2(executable, bundle / executable.name)
    run(bolt, 'clean', app)
    if deployed.exists() or executable.exists():
        raise AssertionError('Project clean did not remove the recorded cross artifacts')
    shutil.rmtree(root)
    if runner and run(runner, bundle / executable.name).strip() != '42':
        raise AssertionError('Relocated application cannot load its shared dependency')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    parser.add_argument('--arch', choices=('x86_64', 'aarch64', 'riscv64'), help='Foreign target architecture')
    parser.add_argument('--sdk', type=Path, help='Target SDK bin/ (enables link tests)')
    parser.add_argument('--sysroot', type=Path, help='Target Linux filesystem or GCC layout with its glibc')
    parser.add_argument('--linker', type=Path, help='Optional cross GNU ld or ld.lld')
    parser.add_argument('--runner', type=Path, help='Optional target runner (e.g. a configured qemu-user wrapper)')
    parser.add_argument('--cxx', type=Path, help='Cross C++ compiler for an additional automatic fallback test')
    args = parser.parse_args()
    if bool(args.sdk) != bool(args.sysroot) or ((args.runner or args.cxx or args.linker) and not args.sdk):
        parser.error('Specify both --sdk and --sysroot for link, run or C++ fallback tests')
    bolt = args.bolt.resolve()
    caps = run(bolt, 'backends')
    target_line = next((line[6:] for line in caps.splitlines() if line.startswith('Host: ')), '')
    host = target_line.partition('-')[0]
    if platform.system() != 'Linux' or not target_line.endswith('-linux-gnu') or host not in ('x86_64', 'aarch64', 'riscv64'):
        raise SystemExit('This suite needs a supported GNU/Linux host')
    arch = args.arch or ('aarch64' if host == 'x86_64' else 'x86_64')
    if arch == host:
        parser.error('--arch must name a foreign architecture')
    machine, symbol = {'aarch64': (183, 'ARM64'), 'x86_64': (62, 'X64'), 'riscv64': (243, 'RISCV64')}[arch]
    triple = arch + '-unknown-linux-gnu'
    have_llvm = 'LLVM: not available' not in caps
    with tempfile.TemporaryDirectory(prefix='Bolt cross build ') as temp:
        work = Path(temp)
        run(bolt, 'init', work / 'Game')
        project = work / 'Game'
        source = next((project / 'src').glob('*.bolt'))
        source.write_text(f'''public static class Program {{ public: void Main() {{
#if Linux && {symbol}
Print("{triple}");
#else
WrongTargetIsActive();
#endif
}} }}\n''')
        run(bolt, 'check', project, '--target', arch + '-linux-gnu')
        run(bolt, 'check', project, error='WrongTargetIsActive')
        run(bolt, 'run', project, '--target', triple, error='foreign Linux target')
        run(bolt, 'compile', project, '--target', 'i686-unknown-linux-gnu', error='unsupported native target')
        run(bolt, 'compile', project, '--target', triple, '--debug', '--no-debug-info', error='requires --runtime-dir')
        assembly = work / 'code.ll'
        run(bolt, 'emit-ir', project, '--target', triple, '-o', assembly)
        if f'target triple = "{triple}"' not in assembly.read_text():
            raise AssertionError('LLVM IR reports the wrong target')
        if arch == 'riscv64':
            run(bolt, 'compile', project, '--debug', '--no-debug-info', '--target', triple,
                '--backend', 'fast', '--object-only', error='no Fast code generator')
            modes = [('auto', '--debug')] if have_llvm else []
        else:
            run(bolt, 'compile', project, '--debug', '--no-debug-info', '--target', triple, '--backend', 'fast', '--object-only')
            elf(project / 'bin' / triple / 'Debug' / 'Game.o', machine, (1,))
            modes = [('fast', '--debug')]
        if have_llvm:
            modes.append(('llvm', '--release'))
        for backend, profile in modes:
            options = [profile, '--target', triple, '--backend', backend, '--require-native', '--no-debug-info']
            obj = work / (backend + '.o')
            run(bolt, 'compile', project, *options, '--object-only', '-o', obj)
            elf(obj, machine, (1,))
            if args.sdk:
                link = ['--runtime-dir', args.sdk.resolve(), '--sysroot', args.sysroot.resolve()]
                if args.linker:
                    link += ['--linker', args.linker.resolve()]
                exe = work / backend / 'game'
                run(bolt, 'compile', project, *options, *link, '-o', exe)
                elf(exe, machine, (2, 3))
                if args.runner and run(args.runner.resolve(), exe).strip() != triple:
                    raise AssertionError('Wrong target-specific program output')
                graph = work / (backend + ' graph')
                exercise_dependencies(bolt, graph, triple, machine,
                                      args.runner.resolve() if args.runner else None,
                                      ['--backend', backend, '--require-native', *link])
        if args.cxx:
            (project / 'ffi.h').write_text('namespace demo { inline int twice(int x) { return 2*x; } }\n')
            source.write_text('''[Header("ffi.h"), Native("demo::twice")] extern "C++" int Twice(int x);
public static class Program { public: void Main() { Print(Twice(21)); } }\n''')
            exe = work / 'cpp' / 'game'
            # Auto needs C++ for this declaration. Expose the supplied compiler
            # under its normal triple-prefixed name so --cxx cannot force cpp.
            alias = work / 'drivers'
            alias.mkdir()
            (alias / (arch+'-linux-gnu-g++')).symlink_to(args.cxx.absolute())
            old_path = os.environ.get('PATH', '')
            os.environ['PATH'] = str(alias) + os.pathsep + old_path
            try:
                run(bolt, 'compile', project, '--debug', '--no-debug-info', '--target', triple,
                    '--runtime-dir', args.sdk.resolve(), '--no-pch', '-o', exe)
                exercise_dependencies(bolt, work, triple, machine,
                                      args.runner.resolve() if args.runner else None,
                                      ['--backend', 'cpp', '--no-pch', '--runtime-dir', args.sdk.resolve()])
            finally:
                os.environ['PATH'] = old_path
            elf(exe, machine, (2, 3))
            if args.runner and run(args.runner.resolve(), exe).strip() != '42':
                raise AssertionError('Wrong C++ FFI result')
        print(f'PASS Linux cross target {triple}, backends {", ".join(b for b, _ in modes)}'
              + (', linked programs' if args.sdk else ', object-only')
              + (', executed on target' if args.runner else '')
              + (', automatic C++ FFI fallback, relocatable static/shared project' if args.cxx else ''))


if __name__ == '__main__':
    main()
