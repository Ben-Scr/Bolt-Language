#!/usr/bin/env python3
"""Verify macro-only inputs exported from the GNU/Clang precompiled header."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--bolt', type=Path, required=True)
    cli.add_argument('--cxx', required=True)
    args = cli.parse_args()
    bolt = args.bolt.resolve()
    with tempfile.TemporaryDirectory(prefix='bolt pch inputs ') as tmp:
        root = Path(tmp)
        project = root / 'project'
        external = root / 'outside'
        project.mkdir()
        external.mkdir()
        include = root / 'SDK' / 'include'
        headers = bolt.parent / 'include'
        # Installed SDKs carry these headers alongside the CLI. The registered
        # CMake test runs the freshly built executable directly from its build tree.
        if not headers.exists() and any((directory / 'CMakeCache.txt').is_file() for directory in bolt.parents):
            headers = Path(__file__).resolve().parents[1] / 'include'
        shutil.copytree(headers, include)
        header = external / 'value.hpp'
        with (include / 'bolt' / 'Std.hpp').open('a') as stream:
            stream.write(f'\n#include "{header.as_posix()}"\n')
        content = ('#if __has_include("enabled")\n#define BOLT_EXTRA 1\n'
                   '#else\n#define BOLT_EXTRA 0\n#endif\n#define BOLT_OUTSIDE 11\n')
        header.write_text(content)
        (project / 'binding.hpp').write_text('inline int value(){return BOLT_OUTSIDE + BOLT_EXTRA;}\n')
        script = project / 'Main.bolt'
        script.write_text('[Header("binding.hpp"),Native("value")] extern "C++" int Value();\n'
                          'public static class Program {public: static void Main(){Print(Value());}}')
        out = root / ('program.exe' if os.name == 'nt' else 'program')
        cmd = [str(bolt), 'compile', str(script), '--backend', 'cpp', '--no-debug-info',
               '--cxx', args.cxx, '--include', str(include), '-o', str(out)]
        environment = {**os.environ, 'BOLT_BUILD_ROOT': str(root / 'build')}

        def build(expected, *, hit=False):
            result = subprocess.run(cmd, cwd=project, env=environment, capture_output=True,
                                    text=True, timeout=300)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert ('Up to date' in result.stdout) == hit, result.stdout
            actual = subprocess.check_output([str(out)], text=True).strip()
            assert actual == str(expected), (expected, actual)
            assert any(root.glob('build/**/bolt_pch.hpp.gch')) or any(root.glob('build/**/bolt_pch.pch'))
            print(f'PASS PCH value {actual}, cache hit {hit}', flush=True)

        build(11)
        build(11, hit=True)
        stamp = header.stat()
        header.write_text(content.replace('OUTSIDE 11', 'OUTSIDE 22'))
        os.utime(header, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        build(22)
        (external / 'enabled').touch()
        build(23)
        build(23, hit=True)
        (include / 'bolt' / 'unused.hpp').write_text('#error Not part of this PCH or program\n')
        build(23, hit=True)
        # A source edit needs its PCH again. It must repair a damaged artifact
        # even when the precompiled header's own size and mtime were preserved.
        pchs = [*root.glob('build/**/bolt_pch.pch'), *root.glob('build/**/bolt_pch.hpp.gch')]
        for pch in pchs:
            stamp = pch.stat()
            with pch.open('r+b') as stream:
                magic = stream.read(8)
                stream.seek(0)
                stream.write(bytes(b ^ 0xff for b in magic))
            os.utime(pch, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        script.write_text(script.read_text().replace('Print(Value())', 'Print(Value() + 1)'))
        build(24)
        build(24, hit=True)

        if os.name != 'nt':
            # A generated header changes during the first PCH compile. Once
            # its former contents are restored, a second target must see those
            # contents even if the PCH compiler saw the newer intermediate file.
            cxx = shutil.which(args.cxx)
            assert cxx
            marker = root / 'header-regenerated'
            wrapper = root / ('changing-' + Path(cxx).name)
            wrapper.write_text(f'#!{sys.executable}\nimport os, sys\nfrom pathlib import Path\n'
                               f'header = Path({str(header)!r})\nmarker = Path({str(marker)!r})\n'
                               'if "c++-header" in sys.argv and not marker.exists():\n'
                               '    marker.touch()\n'
                               '    header.write_text(header.read_text().replace("OUTSIDE 22", "OUTSIDE 44"))\n'
                               f'os.execv({cxx!r}, [{cxx!r}, *sys.argv[1:]])\n')
            wrapper.chmod(0o755)
            previous = header.read_bytes()
            stamp = header.stat()
            cmd[cmd.index('--cxx') + 1] = str(wrapper)
            race_out = root / 'concurrent-input'
            cmd[cmd.index('-o') + 1] = str(race_out)
            result = subprocess.run(cmd, cwd=project, env=environment, check=True,
                                    capture_output=True, text=True, timeout=300)
            assert marker.exists() and 'Up to date' not in result.stdout
            value = subprocess.check_output([str(race_out)], text=True).strip()
            assert value in ('24', '46'), value
            print('PASS generated header changed during a cold PCH build', flush=True)
            header.write_bytes(previous)
            os.utime(header, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            # A different executable forces actual code generation using the
            # shared PCH cache, independently of the first target's receipt.
            out = root / 'fresh-target'
            cmd[cmd.index('-o') + 1] = str(out)
            build(24)
            build(24, hit=True)


if __name__ == '__main__':
    main()
