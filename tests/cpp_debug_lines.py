#!/usr/bin/env python3
"""Check positive C++ DWARF source rows against both real Bolt files."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--bolt', type=Path, required=True)
    cli.add_argument('--cxx', required=True)
    args = cli.parse_args()
    bolt = args.bolt.resolve()
    with tempfile.TemporaryDirectory(prefix='bolt debug rows ') as tmp:
        root = Path(tmp)
        project = root / 'Quelltext ü'
        project.mkdir()
        entry = project / 'Start File.bolt'
        helper = project / 'Rechnen.bolt'
        entry.write_text('public static class Program {\npublic:\n'
                         '    int Main() { Print(Arithmetic.Twice(7)); return 0; }\n}\n')
        helper.write_text('public static class Arithmetic {\npublic:\n'
                          '    int Twice(int value) { return value * 2; } // first\n}\n')
        target = root / 'result'
        env = {**os.environ, 'BOLT_BUILD_ROOT': str(root / 'build')}
        command = [str(bolt), 'compile', str(project), '--backend', 'cpp', '--no-pch',
                   '--debug', '--debug-info', '--cxx', args.cxx, '-o', str(target)]
        def compile_program():
            return subprocess.run(command, check=True, capture_output=True, text=True,
                                  env=env, cwd=root, timeout=240).stdout
        compile_program()
        assert 'Up to date' in compile_program()
        stamp = helper.stat()
        helper.write_text(helper.read_text().replace('// first', '// other'))
        os.utime(helper, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        assert 'Up to date' not in compile_program()
        print('PASS same-timestamp debug source edit with unchanged generated lines')
        assert subprocess.check_output([str(target)], cwd=root, text=True) == '14\n'
        # Independently interpret #line, including synthetic lines the optimizer
        # may choose not to represent in DWARF. The physical .cpp remains inspectable.
        generated = Path(str(target) + '.bolt.cpp')
        directive = re.compile(r'^#line (\d+) "(.*)"$')
        files = {p.as_posix(): len(p.read_text().splitlines()) for p in (entry, helper)}
        marks = {name: set() for name in files}
        virtual_file, row = '', 1
        for physical, text in enumerate(generated.read_text().splitlines(), 1):
            location = directive.fullmatch(text)
            if location:
                row, virtual_file = int(location[1]), location[2]
                continue
            if virtual_file in files:
                assert 0 < row <= files[virtual_file], (virtual_file, row, physical, text)
                marks[virtual_file].add(row)
            if virtual_file == '<bolt generated>':
                assert physical == row, (physical, row)
            row += 1
        assert all(3 in rows for rows in marks.values()), marks
        readelf = shutil.which('readelf')
        if readelf:
            listing = subprocess.check_output([readelf, '--debug-dump=decodedline', str(target)], text=True)
            for path in (entry, helper):
                rows = {int(m[1]) for m in re.finditer(re.escape(path.name) + r'\s+(\d+)\s+0x', listing)}
                # DWARF line zero is the toolchain's conventional unknown location.
                assert 3 in rows and all(n <= files[path.as_posix()] for n in rows), (path, rows)
                print(f'PASS {path.name} known DWARF rows {sorted(rows - {0})}')
        print('PASS source-map ranges, generated physical lines and program output')


if __name__ == '__main__':
    main()
