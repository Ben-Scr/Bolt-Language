#!/usr/bin/env python3
"""File-private declarations use the same checks in check and normal builds."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    args = parser.parse_args()
    bolt = args.bolt.resolve()
    count = 0
    entry = 'static class Program { public: void Main() {} }\n'

    def invoke(root, command='check'):
        result = subprocess.run([str(bolt), command, str(root), '--diagnostics', 'json' if command == 'check' else 'msbuild', '--no-timings'],
                                capture_output=True, text=True, timeout=30)
        if result.returncode and command == 'check':
            payload = next((json.loads(line) for line in result.stdout.splitlines()
                            if line.startswith('{') and 'diagnostics' in line), None)
            assert payload and payload['diagnostics'], (result.stdout, result.stderr)
            return result, payload['diagnostics'][0]
        return result, None

    with tempfile.TemporaryDirectory(prefix='bolt-file-privacy-') as directory:
        base = Path(directory)

        def case(name, declaration, reference, allowed=False, expected='Animal', same_file=False):
            nonlocal count
            root = base / name
            root.mkdir()
            source = root / 'Program.bolt'
            if same_file:
                source.write_text(declaration + '\n' + reference + '\n' + entry, encoding='utf-8')
            else:
                (root / 'Animals.bolt').write_text(declaration + '\n', encoding='utf-8')
                source.write_text(reference + '\n' + entry, encoding='utf-8')
            result, diagnostic = invoke(root)
            if allowed:
                assert result.returncode == 0, (name, result.stdout, result.stderr)
            else:
                assert result.returncode != 0, (name, 'accepted inaccessible declaration')
                assert diagnostic['code'] == 'BLT2005', (name, diagnostic)
                assert diagnostic['phase'] == 'semantics', diagnostic
                assert "private to its declaring file 'Animals.bolt'" in diagnostic['message'], diagnostic
                assert expected in diagnostic['message'], diagnostic
                location = diagnostic['location']
                assert Path(location['file']) == source, diagnostic
                assert location['line'] > 0 and location['column'] > 0 and location['length'] > 0, diagnostic
                assert diagnostic['sourceLine'], diagnostic
                built, _ = invoke(root, 'compile')
                assert built.returncode != 0, (name, built.stdout, built.stderr)
                assert diagnostic['message'] in built.stderr and diagnostic['code'] in built.stderr, (name, diagnostic, built.stderr)
                assert f"({location['line']},{location['column']})" in built.stderr, built.stderr
                assert 'Built Debug' not in built.stdout + built.stderr, built
            count += 1
            print('PASS', name, flush=True)

        for kind in ('class', 'struct', 'enum', 'interface'):
            declaration = 'private ' + kind + ' Animal { ' + ('Idle' if kind == 'enum' else '') + ' }'
            use = 'class Holder { Animal' + ('*' if kind == 'interface' else '') + ' Value; }'
            case(kind + '-same-file', declaration, use, allowed=True, same_file=True)
            case(kind + '-cross-file', declaration, use)
            case(kind + '-default-public', declaration.replace('private ', ''), use, allowed=True)

        case('factory-same-file', 'private class Animal {}',
             'class AnimalFactory { Animal Create() { return Animal(); } }', allowed=True, same_file=True)
        case('namespace-import', 'namespace Zoo; private class Animal {}',
             'import Zoo\nclass Holder { Animal Value; }', expected='Zoo.Animal')
        case('namespace-qualified', 'namespace Zoo; private class Animal {}',
             'class Holder { Zoo.Animal Value; }', expected='Zoo.Animal')
        case('same-namespace-cross-file', 'namespace Zoo; private class Animal {}',
             'namespace Zoo; class Holder { Animal Value; }', expected='Zoo.Animal')
        case('path-import', 'private class Animal {}',
             'import "Animals.bolt"\nclass Holder { Animal Value; }')
        case('generic-argument', 'private class Animal {}',
             'class Box<T> { T Value; } class Holder { Box<Animal> Value; }')
        case('generic-private-type', 'private class Animal<T> { T Value; }',
             'class Holder { Animal<int> Value; }')
        case('unused-generic-body', 'private class Animal {}',
             'class Box<T> { Animal Make() { return Animal(); } }')
        case('unused-generic-expression', 'private class Animal {}',
             'class Box<T> { void Test() { var value = Animal(); } }')
        case('unused-generic-method-expression', 'private class Animal {}',
             'class Box { void Test<T>() { var value = Animal(); } }')
        case('unused-generic-enum-expression', 'namespace Zoo; private enum Animal { Idle }',
             'class Box<T> { void Test() { var value = Zoo.Animal.Idle; } }', expected='Zoo.Animal')
        case('generic-local-shadows-private-type', 'private class Animal {}',
             'class Box<T> { void Test() { int Animal = 1; Print(Animal); } }', allowed=True)
        case('generic-parameter-shadows-private-type', 'private class Animal {}',
             'class Box<T> { void Test(int Animal) { Print(Animal); } }', allowed=True)
        case('generic-shadow-survives-nested-block', 'private class Animal {}',
             'class Box<T> { void Test(int Animal) { { int Animal=1; Print(Animal); } Print(Animal); } }',
             allowed=True)
        case('generic-sibling-branch-scope', 'private class Animal {}',
             'class Box<T> { void Test(bool flag) { if(flag) { int Animal=1; return; } else { Animal(); } } }')
        case('generic-loop-variable-scope', 'private class Animal {}',
             'class Box<T> { void Test() { for(int Animal=0; Animal<2; ++Animal) { Print(Animal); } Animal(); } }')
        case('generic-catch-variable-scope', 'private class Animal {}',
             'class Box<T> { void Test() { try {} catch(Exception Animal) { throw; } Animal(); } }')
        case('generic-next-method-scope', 'private class Animal {}',
             'class Box<T> { void First(int Animal) { Print(Animal); } void Second() { Animal(); } }')
        case('generic-field-shadows-private-type', 'private class Animal {}',
             'class Box<T> { int Animal; void Test() { Print(Animal); } }', allowed=True)
        case('generic-method-parameter-shadows-private-type', 'private class Animal {}',
             'class Holder { Animal Echo<Animal>(Animal value) { return value; } }', allowed=True)
        case('generic-class-parameter-shadows-private-type', 'private class Animal {}',
             'class Holder<Animal> { Animal Value; }', allowed=True)
        case('dependent-method-nested-type-shadows-private-type', 'private class T { public: class Inner {} }',
             'class Factory { T.Inner Make<T>() { return T.Inner(); } }', allowed=True)
        case('dependent-class-nested-type-shadows-private-type', 'private class T { public: class Inner {} }',
             'class Factory<T> { T.Inner Value; }', allowed=True)
        case('inheritance', 'private class Animal {}', 'class Derived : Animal {}')
        case('interface-inheritance', 'private interface Animal {}', 'interface Derived : Animal {}')
        case('constructor-expression', 'private class Animal {}',
             'class Holder { void Test() { Animal(); } }')
        case('static-expression', 'private class Animal { public: static int Value = 3; }',
             'class Holder { int Test() { return Animal.Value; } }')
        case('enum-expression', 'private enum Animal { Idle }',
             'class Holder { void Test() { Animal.Idle; } }')
        case('reflection', '[Reflect] private class Animal {}',
             'import System.Reflection\nclass Holder { void Test() { Reflection.GetType<Animal>(); } }')
        case('task-private-result', 'private class Animal {}',
             'import System.Threading.Tasks\nclass Holder { Task<Animal> Result; }')
        case('thread-private-job', 'private class Animal { public: void Run() {} }',
             'import System.Threading\nclass Holder { void Test() { Thread.Start(Animal()); } }')
        case('nested-public-through-private-owner', 'private class Animal { public: class Cub {} }',
             'class Holder { Animal.Cub Value; }')
        case('nested-same-file-private-owner', 'private class Animal { public: class Cub {} }',
             'class Holder { Animal.Cub Value; }', allowed=True, same_file=True)
        root = base / 'member-private-still-rejected'
        root.mkdir()
        (root / 'Program.bolt').write_text('class Animal { private: int Value; }\n'
            'class Holder { int Read(Animal value) { return value.Value; } }\n' + entry, encoding='utf-8')
        result, diagnostic = invoke(root)
        assert result.returncode != 0 and "'Animal.Value' is private" in diagnostic['message'], diagnostic
        count += 1
        print('PASS member-private-still-rejected', flush=True)

    print(count, 'file visibility checks passed')


if __name__ == '__main__':
    main()
