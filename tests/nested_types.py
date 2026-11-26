"""Nested type frontend regression cases, using the real compiler executable."""
import argparse
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bolt', type=Path, required=True)
args = parser.parse_args()
bolt = args.bolt.resolve()

SUCCESS = {
    'shadowed_generic_parameters': '''
        class Outer<T> {
        public:
            T Value;
            class Inner<T> { public: T Value; }
            T Echo<T>(T value) { return value; }
        }
        static class Program {
        public:
            void Main() {
                Outer<int> outer;
                outer.Value = 17;
                Outer<int>.Inner<string> inner;
                inner.Value = "nested";
                string result = outer.Echo<string>(inner.Value);
            }
        }
    ''',
    'public_class': 'class Outer { public: class Inner { public: int Value; } } static class Program { public: void Main(){ Outer.Inner value; value.Value=3; } }',
    'private_from_owner': 'class Outer { class Hidden {} public: static void Test(){ Hidden value; } } static class Program { public: void Main(){ Outer.Test(); } }',
    'nested_struct': 'class Outer { public: struct Pair { public: int X,Y; Pair(int x,int y){X=x;Y=y;} } } static class Program { public: void Main(){ Outer.Pair value=Outer.Pair(3,4); } }',
    'nested_enum_underlying': 'class Outer { public: enum State : byte { Idle=1, Running=2 } } static class Program { public: void Main(){ Outer.State state=Outer.State.Running; byte value=byte(state); } }',
    'nested_interface_inheritance': 'class Outer { public: interface IValue { int Get(); } class Base { public: virtual int Get(){return 1;} } class Inner : Base,IValue { public: override int Get(){return 2;} } } static class Program { public: void Main(){ Outer.Inner value; Outer.IValue item=value; Print(item.Get()); } }',
    'nested_generic': 'class Outer { public: class Box<T> { public: T Value; Box(T value){Value=value;} } } static class Program { public: void Main(){ Outer.Box<int> value=Outer.Box<int>(3); Print(value.Value); } }',
    'deep_nesting': 'class A { public: class B { public: class C { public: class D {} } } } static class Program { public: void Main(){ A.B.C.D value=A.B.C.D(); } }',
    'top_level_collision': 'class Inner { public: int X; } class Outer { public: class Inner { public: int Y; } static void Test(){ Inner value; value.Y=2; } } static class Program { public: void Main(){ Inner value; value.X=1; Outer.Inner nested; nested.Y=2; Outer.Test(); } }',
    'namespace_resolution': 'namespace Models; class Outer { public: class Inner {} static void Test(){ Inner value; } } static class Program { public: void Main(){ Models.Outer.Inner value; Outer.Test(); } }',
    'generic_outer': 'class Outer<T> { public: class Inner { public: T Value; Inner(T value){Value=value;} } } static class Program { public: void Main(){ Outer<int>.Inner value=Outer<int>.Inner(3); Print(value.Value); } }',
    'generic_outer_and_inner': 'class Outer<T> { public: class Inner<U> { public: T First; U Second; Inner(T first,U second){First=first;Second=second;} } } static class Program { public: void Main(){ Outer<int>.Inner<string> value=Outer<int>.Inner<string>(3,"ok"); Print(value.Second); } }',
    'generic_nested_enum': 'class Outer<T> { public: enum State : T { First=1, Last=2 } } static class Program { public: void Main(){ Outer<byte>.State value=Outer<byte>.State.Last; Print(byte(value)); } }',
    'generic_static_nested_type': 'class Outer<T> { public: static class Inner { public: int Number(){return 7;} } } static class Program { public: void Main(){ Print(Outer<int>.Inner.Number()); } }',
    'generic_qualified_self_type': 'class Outer<T> { public: class Inner { public: T Value; } Outer<T>.Inner Make(T value){ Outer<T>.Inner item; item.Value=value; return item; } } static class Program { public: void Main(){ Outer<int> owner; Outer<int>.Inner item=owner.Make(9); Print(item.Value); } }',
    'generic_qualified_inner_type': 'class Outer<T> { public: class Inner<U> { public: T First; U Second; } Outer<T>.Inner<string> Make(T value){ Outer<T>.Inner<string> item; item.First=value; item.Second="ok"; return item; } } static class Program { public: void Main(){ Outer<int> owner; Outer<int>.Inner<string> item=owner.Make(9); Print(item.Second); } }',
    'dependent_nested_type': 'class TypeHost { public: class Inner { public: int Value=9; } } class Factory<T> { public: T.Inner Make(){return T.Inner();} } static class Program { public: void Main(){ Factory<TypeHost> factory; TypeHost.Inner item=factory.Make(); Print(item.Value); } }',
    'static_owner_instance_nested': 'static class Outer { public: class Inner { public: int Value; } } static class Program { public: void Main(){ Outer.Inner value; value.Value=3; } }',
}
FAILURE = {
    'private_class': ('class Outer { private: class Inner {} } static class Program { public: void Main(){ Outer.Inner value; } }', 'private to its declaring type'),
    'private_struct': ('class Outer { private: struct Inner {} } static class Program { public: void Main(){ Outer.Inner value; } }', 'private to its declaring type'),
    'private_enum': ('class Outer { private: enum State { Idle } } static class Program { public: void Main(){ var value=Outer.State.Idle; } }', 'private to its declaring type'),
    'private_interface': ('class Outer { private: interface Inner {} } static class Program { public: void Main(){ Outer.Inner value; } }', 'private to its declaring type'),
    'private_intermediate_owner': ('class Outer { private: class Middle { public: class Inner {} } } static class Program { public: void Main(){ Outer.Middle.Inner value; } }', 'private to its declaring type'),
    'private_nested_generic': ('class Outer<T> { private: class Inner<U> {} } static class Program { public: void Main(){ Outer<int>.Inner<string> value; } }', 'private to its declaring type'),
    'outer_instance_is_not_implicit': ('class Outer { public: int value; class Inner { public: int Read(){return value;} } } static class Program { public: void Main(){} }', "unknown identifier 'value'"),
    'invalid_nested_enum_underlying': ('class Outer { public: enum State : float { Idle } } static class Program { public: void Main(){} }', 'enum underlying type'),
    'generic_enum_owner_arguments_required': ('class Outer<T> { public: enum State { Idle } } static class Program { public: void Main(){ Outer.State value; } }', 'enclosing generic type'),
    'generic_owner_arguments_required': ('class Outer<T> { public: class Inner {} } static class Program { public: void Main(){ Outer.Inner value; } }', 'enclosing generic type'),
}

with tempfile.TemporaryDirectory(prefix='bolt-nested-types-') as directory:
    root = Path(directory)
    for name, source in SUCCESS.items():
        file = root / (name + '.bolt')
        file.write_text(source, encoding='utf-8')
        result = subprocess.run([str(bolt), 'check', str(file)], capture_output=True, text=True, timeout=40)
        assert result.returncode == 0, (name, result.stdout, result.stderr)
        print('PASS', name, flush=True)
    for name, (source, expected) in FAILURE.items():
        file = root / (name + '.bolt')
        file.write_text(source, encoding='utf-8')
        result = subprocess.run([str(bolt), 'check', str(file)], capture_output=True, text=True, timeout=40)
        assert result.returncode != 0 and expected in result.stderr, (name, expected, result.stdout, result.stderr)
        assert file.as_posix() in result.stderr.replace('\\', '/') and 'error BLT' in result.stderr, (name, result.stderr)
        print('PASS reject', name, flush=True)
    file = root / 'unused_nested_enum.bolt'
    file.write_text('class Outer<T> { public: enum State : byte { Ready=7 } class Inner { public: T Value; } } static class Program { public: void Main(){ Outer<int>.Inner item; } }', encoding='utf-8')
    output = root / 'unused_nested_enum.cpp'
    result = subprocess.run([str(bolt), 'emit', str(file), '-o', str(output), '--no-timings'], capture_output=True, text=True, timeout=40)
    assert result.returncode == 0, (result.stdout, result.stderr)
    emitted = output.read_text(encoding='utf-8')
    import re
    assert re.search(r'Ready\s*=\s*7', emitted), emitted
    print('PASS emitted unused nested enum constants', flush=True)
print(f'{len(SUCCESS) + len(FAILURE) + 1} nested type frontend cases passed', flush=True)
