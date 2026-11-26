#!/usr/bin/env python3
"""Nested type execution across available backends and reflection/JSON metadata."""
import argparse
from pathlib import Path
import subprocess
import tempfile


CORE = '''
class Inner { public: int Value = 2; }
class Outer {
public:
    int Value = 7;
    interface IHelper { int Read(); }
    class Inner : IHelper {
    public:
        int Value = 9;
        int Read() { return Value; }
        int FromOuter(const Outer& owner) { return owner.Value; }
        class Deep { public: int Value = 4; }
    }
    class Derived : Inner { public: int Twice() { return Read() * 2; } }
    struct Pair { public: int X, Y; }
    enum State : byte { Idle = 3, Running = 6 }
    class Box<T> { public: T Value; }
private:
    class Secret { public: int Value = 13; }
public:
    int ReadSecret() { Secret value; return value.Value; }
}
class GenericOuter<T> {
public:
    class Node<U> { public: T First; U Second; }
    class Leaf { public: T Value; }
    enum Mode : byte { Start = 5 }
}
static class Program {
public:
    void Main() {
        Inner root; Outer owner; Outer.Inner nested; Outer.Inner.Deep deep;
        Outer.Derived derived; Outer.IHelper helper = nested;
        Outer.Pair pair; pair.X = 11; pair.Y = 12;
        Outer.State state = Outer.State.Running;
        Outer.Box<int> box; box.Value = 10;
        GenericOuter<int>.Node<string> node; node.First = 21; node.Second = "node";
        GenericOuter<int>.Leaf leaf; leaf.Value = 8;
        GenericOuter<int>.Mode mode = GenericOuter<int>.Mode.Start;
        Print(root.Value); Print(nested.Read()); Print(nested.FromOuter(owner));
        Print(deep.Value); Print(derived.Twice()); Print(helper.Read());
        Print(pair.X + pair.Y); Print((int)state); Print(box.Value); Print(owner.ReadSecret());
        Print(node.First); Print(node.Second); Print(leaf.Value); Print((int)mode);
    }
}
'''

REFLECTION = '''
import System.Reflection
import System.Text.Json
namespace Models;
class Outer {
public:
    [Reflect] class Inner { public: int Value = 17; }
    [Reflect] struct Pair { public: int X = 3; int Y = 4; }
    [Reflect] class Box<T> { public: T Value; }
}
class Host<T> { public: [Reflect] class Leaf { public: T Value; } }
[Reflect] private class Hidden { public: int Value = 19; }
static class Program {
public:
    void Main() {
        Print(Reflection.GetType<Outer.Inner>().Name);
        Print(Reflection.GetType<Outer.Inner>().Namespace);
        Print(Reflection.GetType<Outer.Pair>().Name);
        Print(Reflection.GetType<Outer.Box<int>>().Name);
        Print(Reflection.GetType<Host<int>.Leaf>().Name);
        Print(Reflection.GetType<Hidden>().Name);
        Outer.Inner value; value.Value = 41;
        string json = Json.Serialize<Outer.Inner>(value);
        Outer.Inner copy = Json.Deserialize<Outer.Inner>(json);
        Print(copy.Value);
        Outer.Pair pair; string data = Json.Serialize<Outer.Pair>(pair);
        Outer.Pair pairCopy = Json.Deserialize<Outer.Pair>(data);
        Print(pairCopy.X + pairCopy.Y);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    bolt = args.bolt.resolve()
    available = subprocess.run([str(bolt), 'backends'], capture_output=True, text=True, timeout=30)
    assert available.returncode == 0, available.stderr
    modes = [('interpreter', ['--runtime']), ('cpp', ['--backend', 'cpp', '--cxx', args.cxx])]
    if 'Fast: available' in available.stdout:
        modes += [('fast-jit', ['--backend', 'fast', '--jit', '--require-native']),
                  ('fast-aot', ['--backend', 'fast', '--require-native'])]
    if 'LLVM: not available:' not in available.stdout and 'LLVM: ' in available.stdout:
        modes += [('llvm-jit', ['--backend', 'llvm', '--jit', '--require-native']),
                  ('llvm-release', ['--backend', 'llvm', '--release', '--require-native'])]
    else:
        print('SKIP LLVM backend: compatible shared library unavailable', flush=True)

    with tempfile.TemporaryDirectory(prefix='bolt-nested-backends-') as directory:
        root = Path(directory)
        core = root / 'Core.bolt'
        core.write_text(CORE, encoding='utf-8')
        expected = ['2', '9', '7', '4', '18', '9', '23', '6', '10', '13', '21', 'node', '8', '5']
        for name, options in modes:
            result = subprocess.run([str(bolt), 'run', str(core), *options, '--no-timings'],
                                    capture_output=True, text=True, timeout=180)
            assert result.returncode == 0, (name, result.stdout, result.stderr)
            assert result.stdout.splitlines()[-len(expected):] == expected, (name, result.stdout, result.stderr)
            print('PASS nested execution', name, flush=True)
        reflected = root / 'Reflected.bolt'
        reflected.write_text(REFLECTION, encoding='utf-8')
        expected = ['Models.Outer.Inner', 'Models', 'Models.Outer.Pair', 'Models.Outer.Box<int>',
                    'Models.Host<int>.Leaf', 'Models.Hidden', '41', '7']
        result = subprocess.run([str(bolt), 'run', str(reflected), '--backend', 'cpp', '--cxx', args.cxx,
                                 '--no-timings'], capture_output=True, text=True, timeout=180)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert result.stdout.splitlines()[-len(expected):] == expected, (result.stdout, result.stderr)
        print('PASS nested reflection names, same-file private reflection and JSON roundtrip', flush=True)


if __name__ == '__main__':
    main()
