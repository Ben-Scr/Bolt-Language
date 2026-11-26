"""Exercise the public C ABI through opaque handles, including live module replacement."""
import ctypes as C
import hashlib
import json
import platform
import os
import statistics
import sys
import tempfile
import threading
import time
from pathlib import Path

lib = C.CDLL(str(Path(sys.argv[1]).resolve()))
Handle = C.c_void_p
U64 = C.c_uint64
HostFn = C.CFUNCTYPE(U64, U64, U64)


class Binding(C.Structure):
    _fields_ = [("name", C.c_char_p), ("function", HostFn)]


def bind(name, result, *args):
    fn = getattr(lib, name)
    fn.argtypes, fn.restype = list(args), result
    return fn


errargs = [C.c_char_p, C.c_size_t]
abi = bind("bolt_compiler_host_abi", C.c_uint32)
create = bind("bolt_compiler_create", C.c_int, C.c_uint32, C.c_int, C.POINTER(Binding),
              C.c_size_t, C.POINTER(Handle), *errargs)
destroy = bind("bolt_compiler_destroy", None, Handle)
compile_text = bind("bolt_compiler_compile", C.c_int, Handle, C.c_char_p, C.c_char_p,
                    C.c_size_t, C.POINTER(Handle), *errargs)
compile_project = bind("bolt_compiler_compile_project", C.c_int, Handle, C.c_char_p,
                       C.POINTER(Handle), *errargs)
class Stats(C.Structure):
    _fields_ = [(name, U64) for name in
                ("parsed_files", "reused_files", "frontend_ns", "native_codegen_ns")]


check_project = bind("bolt_compiler_check_project", C.c_int, Handle, C.c_char_p, C.POINTER(Stats), *errargs)
module_stats = bind("bolt_module_stats", C.c_int, Handle, C.POINTER(Stats))
release = bind("bolt_module_release", None, Handle)
load = bind("bolt_compiler_load", C.c_int, Handle, Handle, *errargs)
run = bind("bolt_compiler_run", C.c_int, Handle, C.POINTER(C.c_int32), *errargs)

class Source(C.Structure):
    _fields_ = [("name", C.c_char_p), ("source", C.c_char_p), ("length", C.c_size_t)]


compile_sources = bind("bolt_compiler_compile_sources", C.c_int, Handle, C.POINTER(Source),
                       C.c_size_t, C.c_char_p, C.POINTER(Handle), *errargs)
run_persistent = bind("bolt_compiler_run_persistent", C.c_int, Handle, C.POINTER(C.c_int32), *errargs)


class AttributeInfo(C.Structure):
    _fields_ = [("name", C.c_char_p), ("arguments", C.POINTER(C.c_char_p)), ("count", C.c_size_t)]


class FieldInfo(C.Structure):
    _fields_ = [(key, C.c_char_p) for key in ("owner", "name", "type", "literal")] + [
        ("public", C.c_int), ("static", C.c_int), ("constant", C.c_int),
        ("attributes", C.POINTER(AttributeInfo)), ("count", C.c_size_t)]


FieldVisitor = C.CFUNCTYPE(None, Handle, C.POINTER(FieldInfo))
visit_fields = bind("bolt_compiler_visit_fields", C.c_int, C.POINTER(Source), C.c_int, FieldVisitor, Handle, *errargs)


def explicit_llvm_path_contract():
    with tempfile.TemporaryDirectory() as temp:
        missing = str(Path(temp) / "missing-llvm-library").encode("utf-8")
        configured = bind("bolt_compiler_create_with_llvm", C.c_int, C.c_uint32, C.c_int,
                          C.POINTER(Binding), C.c_size_t, C.c_char_p, C.POINTER(Handle), *errargs)
        host, module, error = Handle(), Handle(), C.create_string_buffer(4096)
        assert configured(abi(), 1, None, 0, missing, C.byref(host), error, len(error)) == 0
        try:
            source = b"public static class Program { public: int Main() { return 1; } }"
            status = compile_text(host, b"explicit.bolt", source, len(source), C.byref(module), error, len(error))
            assert status != 0 and not module.value and b"LLVM" in error.value, error.value
        finally:
            destroy(host)


def field_metadata_contract():
    src = b'namespace Example; class Counter { [Label("Tick")] private int value = -7; }'
    source = Source(b"type.bolt", src, len(src))
    seen = []

    @FieldVisitor
    def visit(_, field):
        item = field.contents
        seen.append((item.owner, item.name, item.type, item.literal, item.public,
                     [(a.name, a.arguments[:a.count]) for a in item.attributes[:item.count]]))

    call(visit_fields, C.byref(source), 0, visit, None)
    assert seen == [(b"Example.Counter", b"value", b"int", b"-7", 0, [(b"Label", [b"Tick"])])]


def async_json_host_contract():
    compiler = host(first, 0)
    source = """import System.Threading.Tasks
import System.Text.Json
[Reflect] class Item { public: int Value { get; set; } }
static class Program {
public:
    async Task<int> Main() {
        await Task.Delay(1);
        Item item; item.Value=42;
        return Json.Deserialize<Item>(Json.Serialize<Item>(item)).Value;
    }
}"""
    try:
        module = build(compiler, source)
        call(load, compiler, module)
        release(module)
        assert run_code(compiler) == 42
        assert run_code(compiler) == 42
    finally:
        destroy(compiler)


def native_fixture_contract(profile, name):
    source = (Path(__file__).parent / "fixtures" / name).read_text(encoding="utf-8")
    compiler = host(first, profile)
    try:
        module = build(compiler, source)
        try:
            call(load, compiler, module)
        finally:
            release(module)
        assert run_code(compiler) == 0
    finally:
        destroy(compiler)


def method_edit_contract():
    delta_count = bind("bolt_module_incremental_methods", C.c_size_t, Handle)
    imports = b'extern "C" ulong FunkeValue(ulong key,ulong amount);'
    base = """public static class Entry {
    public static int Unchanged() {return 17;}
    public static int Main() {
        int x=(int)FunkeValue(0ul,3ul);
        return EXPR;
    }
HELPERS}
"""
    def script(expr, helpers=""):
        return base.replace("EXPR", expr).replace("HELPERS", helpers)

    def compile_unit(host, text, expected=0):
        raw = text.encode()
        units = (Source * 2)(Source(b"bindings.bolt", imports, len(imports)), Source(b"class.bolt", raw, len(raw)))
        image = Handle()
        message = call(compile_sources, host, units, 2, b"Entry", C.byref(image), expected=expected)
        return image, message

    compiler = host(first, 0)
    images = []
    def installed(text, count, result):
        image, _ = compile_unit(compiler, text)
        images.append(image)
        assert delta_count(image) == count, (delta_count(image), count, text)
        call(load, compiler, image)
        assert run_code(compiler) == result
        return image
    try:
        original = installed(script("x + 1"), 0, 5)
        updated = installed(script("(x * x) - 3"), 1, 13)
        helper = "    private static int LocalProduct(int x) { if(x>2)return x*x; return x; }\n"
        with_helper = installed(script("LocalProduct(x)", helper), 2, 16)
        revised = "    private static int LocalOffset(int x,int y) { return x-y; }\n"
        installed(script("LocalOffset(x,3)", revised), 2, 1)
        call(load, compiler, original)
        assert run_code(compiler) == 5
        call(load, compiler, with_helper)
        assert run_code(compiler) == 16
        broken = script("LocalOffset(x)", revised)
        image, error = compile_unit(compiler, broken, expected=1)
        assert not image.value and error and run_code(compiler) == 16

        clean = host(first, 0)
        try:
            assert compile_unit(clean, broken, expected=1)[1] == error
        finally:
            destroy(clean)
        # Start a new baseline with a private declaration already present.
        prior, _ = compile_unit(compiler, script("LocalProduct(x)", helper))
        images.append(prior)
        installed(script("LocalProduct(x)", helper.replace("return x*x", "return x+5")), 1, 9)
        widened = helper.replace("LocalProduct(int x)", "LocalProduct(long x)").replace("return x*x", "return (int)x+3").replace("return x;", "return (int)x;")
        installed(script("LocalProduct(x)", widened), 2, 7)
        installed(script("LocalOffset(x,9)", revised), 2, -5)
        # A layout edit must be treated as a complete new class contract.
        changed_layout = script("x+2").replace("public static int Unchanged", "public static int Extra;\n    public static int Unchanged")
        installed(changed_layout, 0, 6)
        call(load, compiler, updated)
        assert run_code(compiler) == 13

        old_source = script("x + 1").replace("FunkeValue(0ul", "FunkeValue(77ul")
        old, _ = compile_unit(compiler, old_source)
        images.append(old)
        call(load, compiler, old)
        entered.clear()
        proceed.clear()
        results = []
        worker = threading.Thread(target=lambda: results.append(run_code(compiler)), daemon=True)
        worker.start()
        try:
            assert entered.wait(20)
            new, _ = compile_unit(compiler, old_source.replace("x + 1", "x * x"))
            images.append(new)
            assert delta_count(new) == 1
            call(load, compiler, new)
        finally:
            proceed.set()
            worker.join(30)
        assert results == [5] and run_code(compiler) == 16
    finally:
        for image in images:
            release(image)
        destroy(compiler)


def fast_edit_contract():
    compiler = host(first, 0)
    old, new = Handle(), Handle()
    data = b"public static class Count { public: static int Hits = 0; }"
    def compile_value(src, expected=0):
        source = src.encode()
        units = (Source * 2)(Source(b"data.bolt", data, len(data)), Source(b"step.bolt", source, len(source)))
        module = Handle()
        call(compile_sources, compiler, units, len(units), b"Entry", C.byref(module), expected=expected)
        return module

    def step():
        value = C.c_int32()
        call(run_persistent, compiler, C.byref(value))
        return value.value

    def info(module):
        stats = Stats()
        assert module_stats(module, C.byref(stats)) == 0
        return stats

    template = "public static class Entry { public: int Main() { Count.Hits += %s; int extra=%s; return Count.Hits + extra; } }"
    try:
        old = compile_value(template % (1, 2))
        call(load, compiler, old)
        assert [step(), step()] == [3, 4]
        new = compile_value(template % (123, 45))
        assert info(new).parsed_files == 0
        call(load, compiler, new)
        assert [step(), step()] == [168, 291]
        call(load, compiler, old)
        assert step() == 5
        assert not compile_value(template % ("unknown", 45), expected=1)
        assert step() == 6
        other = compile_value(template.replace("+ extra", "- extra") % (123, 45))
        assert info(other).parsed_files > 0
        call(load, compiler, other)
        release(other)
        assert step() == 78

        failing = "public static class Entry { public: int Main() { int[] a=new int[1]; int n=%s; return a[n]; } }"
        base = compile_value(failing % 1)
        release(base)
        edited = compile_value(failing % 234)
        assert info(edited).parsed_files == 0
        call(load, compiler, edited)
        message = call(run, compiler, C.byref(C.c_int32()), expected=1)
        release(edited)
        fresh = host(first, 0)
        try:
            raw = (failing % 234).encode()
            units = (Source * 2)(Source(b"data.bolt", data, len(data)), Source(b"step.bolt", raw, len(raw)))
            full = Handle()
            call(compile_sources, fresh, units, 2, b"Entry", C.byref(full))
            call(load, fresh, full)
            release(full)
            assert b"int n=234" in message and call(run, fresh, C.byref(C.c_int32()), expected=1) == message
        finally:
            destroy(fresh)
    finally:
        release(old)
        release(new)
        destroy(compiler)


def persistent_sources_contract():
    compiler = host(first, 0)
    helper = b"public static class Count { public: static int Value = 0; }"
    entry = b"public static class Entry { public: int Main() { Count.Value += 1; return Count.Value; } }"
    sources = (Source * 2)(Source(b"data.bolt", helper, len(helper)), Source(b"entry.bolt", entry, len(entry)))

    def install():
        module = Handle()
        call(compile_sources, compiler, sources, 2, b"Entry", C.byref(module))
        call(load, compiler, module)
        release(module)

    def step():
        code = C.c_int32()
        call(run_persistent, compiler, C.byref(code))
        return code.value

    try:
        install()
        assert [step(), step()] == [1, 2]
        invalid = Source(b"broken.bolt", b"class {", 7)
        missing = Handle()
        assert b"broken.bolt" in call(compile_sources, compiler, C.byref(invalid), 1,
                                       b"Entry", C.byref(missing), expected=1)
        assert not missing.value and step() == 3
        install()
        assert step() == 1
        install()
        assert [run_code(compiler), run_code(compiler)] == [1, 1]
    finally:
        destroy(compiler)


entered, proceed = threading.Event(), threading.Event()


@HostFn
def first(entity, value):
    if entity == 77:
        entered.set()
        proceed.wait(30)
    return value + 1


@HostFn
def second(entity, value):
    return value + 2


def call(fn, *args, expected=0):
    error = C.create_string_buffer(4096)
    status = fn(*args, error, len(error))
    assert status == expected, (fn.__name__, status, error.value)
    return error.value


def host(fn, profile):
    handle = Handle()
    b = Binding(b"FunkeValue", fn)
    call(create, abi(), profile, C.byref(b), 1, C.byref(handle))
    assert handle.value
    return handle


def build(compiler, src, expected=0):
    image = Handle()
    text = src.encode()
    message = call(compile_text, compiler, b"host-test.bolt", text, len(text),
                   C.byref(image), expected=expected)
    if expected:
        assert message and not image.value, message
    return image


def run_code(compiler):
    code = C.c_int32()
    call(run, compiler, C.byref(code))
    return code.value


def body(value, entity=0):
    return ('extern "C" ulong FunkeValue(ulong entity, ulong value);\n'
            'public static class Program { public: int Main() { return (int)'
            f'FunkeValue({entity}ul, {value}ul); }} }}')


def incremental_frontend_contract(profile):
    compiler = host(first, profile)
    try:
        def edit(source, result):
            module = build(compiler, source)
            call(load, compiler, module)
            release(module)
            assert run_code(compiler) == result

        prefix = "public static class Program { public: int Main() { return Twice(6); } "
        def script(params, body):
            return prefix + f"static int Twice({params}) {{ {body} }} }}"
        edit(script("int x", "return x + 6;"), 12)
        edit(script("int x", "int same=6; if (x==6) return same*x; return 6;"), 36)
        # All old callers must be rebound when the called declaration changes.
        build(compiler, script("int x, int y", "return x + y;"), expected=1)
        assert run_code(compiler) == 36
        edit(script("int x, int y", "return x + y;").replace("Twice(6)", "Twice(6,3)"), 9)
        # An overload-induced range error must retain the previous runnable image.
        build(compiler, script("byte x", "return x;").replace("Twice(6)", "Twice(256)"), expected=1)
        assert run_code(compiler) == 9
        edit(script("int x", "return x - 6;"), 0)
        build(compiler, script("int x", "if (x>2) return x;"), expected=1)
        assert run_code(compiler) == 0
        # Richer syntax crosses the scalar subset boundary in both directions.
        edit(script("int x", "int[] a = [x,7]; return a[0]+a[1];"), 13)
        body = "int[] a = [x]; int offset=12; a[0]=23; return a[0]+offset;"
        edit(script("int x", body), 35)
        edit(script("int x", body.replace("12", "1234")), 1257)
        edit(script("int x", body.replace("23;", "42;")), 54)
        build(compiler, script("int x", body.replace("12", "undefined")), expected=1)
        assert run_code(compiler) == 54
        edit(script("int x", "return (int)(x+1);"), 7)
        # Changing Main's signature must pass the full entry-point checks.
        build(compiler, script("int x", "return x;").replace("int Main()", "int Main(int a)"), expected=1)
        assert run_code(compiler) == 7
    finally:
        destroy(compiler)


def compare_incremental_with_fresh():
    persistent = host(first, 0)
    try:
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            entry, rules, alternate = [root / name for name in ("Entry.bolt","Rules.bolt","Alternate.bolt")]
            def main(dep, args):
                return f'import "{dep}"; public static class Program {{ public: int Main() {{return Rules.Value({args});}} }}'
            def methods(first_param, first_body, other_body):
                return ("public static class Rules { public: "
                        f"static int Value({first_param}) {{ {first_body} }} "
                        f"static int Value(long x) {{ {other_body} }} }}")

            last = None
            def compare(wanted, *, runtime_error=False):
                nonlocal last
                cold = host(first, 0)
                try:
                    messages = []
                    for compiler in (persistent, cold):
                        module = Handle()
                        error = C.create_string_buffer(4096)
                        result = compile_project(compiler, os.fsencode(entry), C.byref(module), error, len(error))
                        assert result == (1 if wanted is None else 0), (wanted, result, error.value)
                        messages.append(error.value)
                        if wanted is None:
                            assert not module.value and error.value
                        else:
                            call(load, compiler, module)
                            release(module)
                            if runtime_error:
                                messages[-1] = call(run, compiler, C.byref(C.c_int32()), expected=1)
                                assert wanted in messages[-1], messages[-1]
                            else:
                                assert run_code(compiler) == wanted
                    assert messages[0] == messages[1], messages
                    if wanted is None:
                        assert run_code(persistent) == last
                    else:
                        last = wanted
                    return messages[0]
                finally:
                    destroy(cold)

            entry.write_text(main("Rules.bolt","9"))
            rules.write_text(methods("int x","return x+2;","return 80;"))
            compare(11)
            # A changed arity rebinds both overloads and every compiled caller.
            rules.write_text(methods("int x, int y","return x-y;","return 80;"))
            compare(80)
            entry.write_text(main("Rules.bolt","9,4"))
            compare(5)
            rules.write_text(methods("int x, int y","return missing;","return 80;"))
            compare(None)
            rules.write_text(methods("int x, int y","if (x>y) return x*y; return y;","return 80;"))
            compare(36)
            # New import closure with the same class and method names.
            alternate.write_text(methods("int x, int y","return x+y;","return 20;"))
            entry.write_text(main("Alternate.bolt","9,4"))
            compare(13)
            alternate.unlink()
            compare(None)
            entry.write_text(main("Rules.bolt","9,4"))
            compare(36)
            # Shrinking an operator shifts a later declaration in the same file.
            # Rebuild that method's positions and retain the reachable library calls.
            data = root / 'sample.bin'
            data.write_bytes(b'\x07')
            script = ('import System.IO\npublic static class Program { public:\n'
                      'bool Test(const byte[]& data) { return data[0] >= 7; }\n'
                      f'int Main() {{ byte[] data = File.ReadAllBytes("{data.as_posix()}"); '
                      'if (Test(data)) return 1; return 0; } }')
            entry.write_text(script)
            compare(1)
            strict = script.replace('>= 7', '> 7')
            entry.write_text(strict)
            compare(0)
            entry.write_text(strict.replace('Test(data)', 'Test(42)'))
            compare(None)
            entry.write_text(script)
            compare(1)
            # Switching to a previously unreachable library method must analyze
            # its transitive callees before the next generation can execute.
            entry.write_text(script.replace('if (Test(data)) return 1;',
                                            f'if (File.Exists("{data.as_posix()}")) return 4;'))
            compare(4)
            # Both edits trap at the same subscript on a line after the helper
            # body begins. Its byte column shifts with the operator width.
            trap = script.replace('}\nint Main', '} int Main').replace('if (Test(data))', 'if (data[1] > 0 || Test(data))')
            for revision in (trap, trap.replace('>= 7', '> 7')):
                entry.write_text(revision)
                compare(b'BLT3002', runtime_error=True)
            # Compilation still distinguishes method type parameters from
            # file-private types when no editor reference index is requested.
            rules.write_text('private class Hidden {}')
            generic = ('import "Rules.bolt"; public static class Program { public: '
                       'void Keep<Hidden>(Hidden value) {} int Main() { return 9; } }')
            entry.write_text(generic)
            compare(9)
            entry.write_text(generic.replace('Keep<Hidden>', 'Keep'))
            assert b'BLT2005' in compare(None)
    finally:
        destroy(persistent)


def exercise(profile, cycles):
    a, b = host(first, profile), host(second, profile)
    try:
        call(run, a, C.byref(C.c_int32()), expected=2)
        x = build(a, body(40, 77))
        call(load, a, x)
        assert b"another compiler" in call(load, b, x, expected=1)
        release(x)
        results = []
        worker = threading.Thread(target=lambda: results.append(run_code(a)), daemon=True)
        entered.clear()
        proceed.clear()
        worker.start()
        try:
            assert entered.wait(20), "old generation did not enter callback"
            x = build(a, body(41))
            call(load, a, x)
            release(x)
            assert run_code(a) == 42
            y = build(b, body(41))
            call(load, b, y)
            release(y)
            assert run_code(b) == 43 and run_code(a) == 42
            build(a, 'static class Program {', expected=1)
            build(a, 'extern "C" int FunkeValue(int a); static class Program { public: int Main(){return FunkeValue(1);} }', expected=1)
            build(a, 'extern "C" long FunkeValue(long a, long b); static class Program { public: int Main(){return (int)FunkeValue(1l,2l);} }', expected=1)
            assert run_code(a) == 42
            call(load, a, None)
            call(run, a, C.byref(C.c_int32()), expected=2)
        finally:
            proceed.set()
            worker.join(30)
        assert results == [41], results
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "host.bolt"
            path.write_text(body(18), encoding="utf-8")
            compiled = Handle()
            call(compile_project, a, os.fsencode(path), C.byref(compiled))
            call(load, a, compiled)
            release(compiled)
            assert run_code(a) == 19
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "Main.bolt").write_text(
                'public static class Program { public: int Main(){return Helper.Read();} }',
                encoding="utf-8")
            helper = root / "Helper.bolt"
            def write_helper(value):
                helper.write_text(f'public static class Helper {{ public: int Read(){{return {value};}} }}',
                                  encoding="utf-8")
            def use_project(code):
                module = Handle()
                call(compile_project, a, os.fsencode(root), C.byref(module))
                info = Stats()
                assert module_stats(module, C.byref(info)) == 0
                assert info.frontend_ns and info.native_codegen_ns
                call(load, a, module)
                release(module)
                assert run_code(a) == code
                return info
            write_helper(31)
            checked = Stats()
            call(check_project, a, os.fsencode(root), C.byref(checked))
            assert checked.frontend_ns and checked.native_codegen_ns == 0
            assert use_project(31).parsed_files == 0
            assert use_project(31).parsed_files == 0
            stamp = helper.stat().st_mtime_ns
            write_helper(32)
            os.utime(helper, ns=(stamp, stamp))
            changed = use_project(32)
            assert changed.parsed_files == 1 and changed.reused_files >= 2
            for broken in ['public static class Helper {',
                           'public static class Helper { public: int Read(){return missing;} }']:
                helper.write_text(broken, encoding="utf-8")
                call(check_project, a, os.fsencode(root), None, expected=1)
                assert run_code(a) == 32
                write_helper(32)
                recovered = use_project(32)
                assert recovered.parsed_files == 0 and recovered.reused_files >= 2
            helper.unlink()
            call(check_project, a, os.fsencode(root), None, expected=1)
            missing = Handle()
            call(compile_project, a, os.fsencode(root), C.byref(missing), expected=1)
            assert not missing.value and run_code(a) == 32
        times = []
        phases = []
        rss = []
        record = bool(os.environ.get("BOLT_RELOAD_REPORT"))
        for i in range(cycles):
            text = body(i)
            start = time.perf_counter_ns()
            image = build(a, text)
            call(load, a, image)
            try:
                assert run_code(a) == i + 1
                elapsed = (time.perf_counter_ns() - start) / 1e6
                if record:
                    info = Stats()
                    assert module_stats(image, C.byref(info)) == 0
                    phases.append([info.parsed_files, info.reused_files, info.frontend_ns, info.native_codegen_ns])
            finally:
                release(image)
            times.append(elapsed)
            if record and (i == 0 or (i + 1) % 100 == 0) and Path("/proc/self/statm").is_file():
                pages = int(Path("/proc/self/statm").read_text().split()[1])
                rss.append([i + 1, pages * os.sysconf("SC_PAGE_SIZE")])
        if times:
            print(f"profile={profile} native replacements={len(times)} "
                  f"p50={statistics.median(times):.2f} ms "
                  f"p95={sorted(times)[(95*len(times)+99)//100-1]:.2f} ms", flush=True)
        return {"profile": profile, "cycles": len(times), "edit_to_return_ms": times,
                "phases": phases, "rss_bytes_after_cycle": rss}
    finally:
        proceed.set()
        destroy(a)
        destroy(b)


if __name__ == "__main__":
    assert abi() == 1
    h = Handle()
    assert b"ABI" in call(create, 0, 0, None, 0, C.byref(h), expected=1)
    assert not h.value
    field_metadata_contract()
    async_json_host_contract()
    native_fixture_contract(0, "HostedTaskCompletion.bolt")
    native_fixture_contract(0, "OwnedSlotLifetime.bolt")
    persistent_sources_contract()
    fast_edit_contract()
    method_edit_contract()
    incremental_frontend_contract(0)
    compare_incremental_with_fresh()
    results = [exercise(0, int(os.environ.get("BOLT_RELOAD_CYCLES", 3)))]
    if os.environ.get("BOLT_TEST_LLVM_HOST") == "1":
        explicit_llvm_path_contract()
        native_fixture_contract(1, "HostedTaskCompletion.bolt")
        native_fixture_contract(1, "OwnedSlotLifetime.bolt")
        results.append(exercise(1, 1))
    if target := os.environ.get("BOLT_RELOAD_REPORT"):
        path = Path(sys.argv[1]).resolve()
        report = {"system": platform.platform(), "library": path.name,
                  "library_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                  "clock": "perf_counter_ns", "unit": "ms",
                  "scope": "changed in-memory source to first completed call, persistent compiler, full JIT image",
                  "phase_columns": ["parsed_files","reused_files","frontend_ns","native_codegen_ns"],
                  "results": results}
        Path(target).write_text(json.dumps(report, separators=(",", ":")) + "\n", encoding="utf-8")
    print("PASS compiler C ABI, host bindings, project compilation, rollback, unload, concurrent reload")
