#!/usr/bin/env python3
"""Check extension visibility and invocation across source-file boundaries."""
import argparse
import subprocess
from pathlib import Path

from lsp_intellisense import fixture_directory


MODELS = 'namespace Models; public class Widget {public:int Value=3;}\n'
METHOD = 'int Twice(this const Widget& value){return value.Value*2;}'
EXTENSIONS = 'import Models\nnamespace Helpers; public static class Extensions {public:' + METHOD + '}\n'


def program(imports='', namespace='', expression='value.Twice()'):
    return ('import Models\n' + imports + namespace +
            'static class Program {public:void Main(){Widget value;Print(' + expression + ');}}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    parser.add_argument('--temp-root', type=Path)
    args = parser.parse_args()
    bolt = args.bolt.resolve()
    count = 0
    listing = subprocess.run([str(bolt), 'backends'], capture_output=True, text=True, timeout=20)
    assert listing.returncode == 0, (listing.stdout, listing.stderr)
    native = 'Fast: available' in listing.stdout
    if not native:
        print('SKIP runtime invocations: Fast JIT is unavailable; checking source scopes only', flush=True)

    with fixture_directory(args.temp_root) as base:
        if native:
            probe = base / 'Probe.bolt'
            probe.write_text('static class Program {public:int Main(){return 0;}}', encoding='utf-8')
            baseline = subprocess.run([str(bolt), 'run', str(probe), '--jit', '--backend', 'fast',
                                       '--require-native', '--no-debug-info', '--no-timings'],
                                      capture_output=True, text=True, timeout=30)
            if baseline.returncode and 'uses native C/C++ interoperability' in baseline.stderr:
                native = False
                print('SKIP runtime invocations: minimal program is blocked by the installed stdlib interoperability guard', flush=True)
            else:
                assert baseline.returncode == 0, (baseline.stdout, baseline.stderr)

        def case(name, source, extensions=EXTENSIONS, expected='6\n', error='', extra=None):
            nonlocal count
            root = base / name
            root.mkdir()
            (root / 'bolt.project').write_text('{}', encoding='utf-8')
            (root / 'Models.bolt').write_text(MODELS, encoding='utf-8')
            (root / 'Extensions.bolt').write_text(extensions, encoding='utf-8')
            (root / 'Program.bolt').write_text(source, encoding='utf-8')
            for filename, content in (extra or {}).items():
                (root / filename).write_text(content, encoding='utf-8')
            checked = subprocess.run([str(bolt), 'check', str(root), '--no-timings'],
                                     capture_output=True, text=True, timeout=30)
            if error:
                assert checked.returncode != 0, (name, 'unexpectedly accepted', checked.stdout)
                assert error in checked.stdout + checked.stderr, (name, checked.stdout, checked.stderr)
            else:
                assert checked.returncode == 0, (name, checked.stdout, checked.stderr)
                if native:
                    ran = subprocess.run([str(bolt), 'run', str(root), '--jit', '--backend', 'fast',
                                          '--require-native', '--no-debug-info', '--no-timings'],
                                         capture_output=True, text=True, timeout=30)
                    assert ran.returncode == 0, (name, ran.stdout, ran.stderr)
                    assert ran.stdout.endswith(expected), (name, ran.stdout, expected)
            count += 1
            print('PASS', name, flush=True)

        case('import-namespace', program('import Helpers\n'))
        case('import-declaring-type', program('import Helpers.Extensions\n'))
        case('same-namespace', program(namespace='namespace Helpers;\n'))
        case('explicit-static-call', program(expression='Helpers.Extensions.Twice(value)'))
        case('unimported-namespace', program(), error='has no member')
        case('path-import-does-not-import-namespace', program('import "Extensions.bolt"\n'),
             error='has no member')
        case('path-and-namespace-import', program('import "Extensions.bolt"\nimport Helpers\n'))
        case('private-owner-hidden', program('import Helpers\n'),
             extensions=EXTENSIONS.replace('public static class', 'private static class'),
             error='has no member')
        case('private-method-hidden', program('import Helpers\n'),
             extensions=EXTENSIONS.replace('{public:', '{private:'), error='has no member')
        case('ambiguous-imported-extensions', program('import Helpers\n'),
             extra={'Second.bolt': EXTENSIONS.replace('class Extensions', 'class Other')},
             error='ambiguous method overload')
        case('private-overload-does-not-compete', program('import Helpers\n'),
             extra={'Second.bolt': EXTENSIONS.replace('class Extensions', 'class Other').replace('{public:', '{private:')})
        case('global-top-level-across-files', program(), extensions='import Models\n' + METHOD)
        case('namespaced-top-level-imported', program('import Helpers\n'),
             extensions='import Models\nnamespace Helpers;\n' + METHOD)
        case('namespaced-top-level-not-imported', program(),
             extensions='import Models\nnamespace Helpers;\n' + METHOD, error='has no member')
        case('generic-receiver-across-files', program('import Helpers\n', expression='value.Echo().Value'),
             extensions='namespace Helpers; public static class Extensions {public:T Echo<T>(this T value){return value;}}',
             expected='3\n')

    print(count, 'extension scope checks passed')


if __name__ == '__main__':
    main()
