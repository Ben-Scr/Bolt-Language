#!/usr/bin/env python3
"""Exercise the artifact ABI of separately compiled Bolt projects."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(*command):
    result = subprocess.run(command, text=True, encoding="utf-8", errors="replace",
                            capture_output=True, timeout=240)
    if result.returncode:
        raise AssertionError(f"{command}\n{result.stdout}{result.stderr}")
    return result.stdout


def project(root, name, kind, source, dependencies=()):
    path = root / name
    (path / "src").mkdir(parents=True)
    manifest = f'format=1\n[project]\nname="{name}"\nkind="{kind}"\n'
    if dependencies:
        manifest += "[dependencies]\n"
        for dep in dependencies:
            manifest += f'{dep}={{path="../{dep}"}}\n'
    (path / "bolt.proj").write_text(manifest)
    (path / "src/Main.bolt").write_text(source)
    return path


def project_artifact(bolt, path, profile):
    info = run(bolt, "project", "info", path, "--profile", profile, "--format", "json")
    return Path(json.loads(info)["artifact"])


def assert_runtime_free(*artifacts):
    for path in artifacts:
        image = path.read_bytes().lower()
        assert all(name not in image for name in (b"boltruntime.dll\0", b"libboltruntime.so\0", b"libboltruntime.dylib\0")), path


def exercise(bolt, backend, root):
    profile = "Release" if backend == "llvm" else "Debug"
    flags = ["--profile", profile, "--backend", backend, "--require-native", "--no-debug-info"]
    base = project(root, "Base", "static-library", """
public abstract class Shape { public: abstract int Score(); }
public static class Counter {
public:
    int Total = 2;
    int Next() { return ++Total; }
    int Evaluate(Shape& shape) { return shape.Score(); }
    T Echo<T>(T value) { return value; }
}
""")
    source = """
public static class Scores {
public:
    int Value(Shape& shape) { return 10 + Counter.Next() + Counter.Evaluate(shape); }
}
"""
    shared = project(root, "Scores", "shared-library", source, ["Base"])
    project(root, "Bridge", "static-library", """
public static class Bridge { public: int Value(Shape& shape) { return Scores.Value(shape); } }
""", ["Scores"])
    app = project(root, "Game", "executable", """
public class Derived : Shape { public: override int Score() { return 20; } }
public static class Program {
public:
    void Main() {
        Derived shape;
        Print(Bridge.Value(shape));
        Print(Counter.Echo<int>(Counter.Next()));
    }
}
""", ["Bridge"])

    def artifact(path):
        return project_artifact(bolt, path, profile)

    # Build the archive independently before compiling its two consumers.
    run(bolt, "compile", base, *flags)
    with artifact(base).open("rb") as archive:
        assert archive.read(8) == b"!<arch>\n"
    assert run(bolt, "run", app, *flags).endswith("33\n4\n")
    dll = artifact(shared)
    exe = artifact(app)
    deployed = exe.parent / dll.name
    assert deployed.read_bytes() == dll.read_bytes()
    deployed.unlink()
    if os.name == "nt":
        dll.with_suffix(".lib").unlink()
    assert run(bolt, "run", app, *flags).endswith("33\n4\n")
    assert run(bolt, "run", app, "--profile", profile, "--no-compile", "--require-native").endswith("33\n4\n")

    # Also exercise automatic backend selection on a project graph.
    assert run(bolt, "run", app, "--profile", profile, "--require-native", "--no-debug-info").endswith("33\n4\n")

    # Replace only the dynamic library. The original executable must call its new body.
    (shared / "src/Main.bolt").write_text(source.replace("return 10", "return 40"))
    run(bolt, "compile", shared, *flags)
    assert run(exe) == "33\n4\n"
    shutil.copy2(dll, deployed)
    assert run(exe) == "63\n4\n"
    templates = project(root, "Templates", "shared-library",
                        "public static class Identity<T> { public: T Echo(T value) { return value; } }")
    run(bolt, "compile", templates, *flags)
    assert artifact(templates).is_file()
    if os.name == "nt":
        assert artifact(templates).with_suffix(".lib").is_file()
    print(f"PASS {backend} static and shared artifacts, transitive dispatch, shared state, generics and replacement")


def exercise_runtime_free(bolt, root):
    source = """
public abstract class Step { public: abstract int Amount(); }
public struct Scratch { public: ulong A; ulong B; ulong C; ulong D; int Last; }
public class Seed {
public:
    int Value;
    Seed(int n) { Value = n; }
}
public static class Leaf {
public:
#if LEAF_BUILD
    Seed State = Seed(20);
#else
    MissingLibraryDefine State;
#endif
    int Next(Step& step) { Scratch fresh; State.Value += fresh.Last + step.Amount(); return State.Value; }
    T Echo<T>(T value) { return value; }
    int Hosted() { return Runtime.ExecuteProgram("invalid"); }
}
"""
    leaf = project(root, "Leaf", "static-library", source)
    (leaf / "bolt.conf").write_text('format=1\n[language]\ndefines=["LEAF_BUILD"]\n')
    for name in ("Left", "Right"):
        project(root, name, "static-library",
                f"public static class {name} {{ public: int Value(Step& s) {{ return Leaf.Echo<int>(Leaf.Next(s)); }} }}",
                ["Leaf"])
    app = project(root, "App", "executable", """
public class Increment : Step { public: override int Amount() { return 1; } }
public static class Program { public: void Main() { Increment s; Print(Left.Value(s) + Right.Value(s)); } }
""", ["Left", "Right"])
    sdk = root / "minimal-sdk"
    sdk.mkdir()
    start = bolt.parent / "BoltStart.o"
    if start.is_file():
        shutil.copy2(start, sdk / start.name)
    flags = ["--no-runtime", "--runtime-dir", sdk]
    command = [bolt, "compile", app, *flags]
    # A separately built hosted archive can already exist for this source project.
    run(bolt, "compile", leaf, "--release", "--backend", "llvm")
    for profile in ("Debug", "Release"):
        run(*command, "--profile", profile)
        exe = Path(json.loads(run(bolt, "project", "info", app, "--profile", profile, "--format", "json"))["artifact"])
        assert not any("boltruntime" in path.name.lower() for path in exe.parent.iterdir())
        assert_runtime_free(exe)
        assert run(exe) == "43\n"
        assert "Up to date" in run(*command, "--profile", profile)
        assert run(bolt, "run", app, "--profile", profile, "--no-compile", "--no-runtime").endswith("43\n")

    code = leaf / "src/Main.bolt"
    code.write_text(source.replace("Seed(20)", "Seed(30)"))
    assert run(bolt, "run", app, "--release", *flags).endswith("63\n")
    hosted_initializer = """
public class HostedSeed { public: HostedSeed() { Leaf.Hosted(); } }
public static class Startup { public: HostedSeed Unused = HostedSeed(); }
"""
    for change in (source.replace("return State.Value;", "return Hosted();"), source + hosted_initializer):
        code.write_text(change)
        failed = subprocess.run([*command, "--release"], text=True, capture_output=True, timeout=240)
        assert failed.returncode and "--no-runtime" in failed.stderr and "Leaf.Hosted" in failed.stderr, failed.stderr
        assert run(bolt, "run", app, "--release", "--no-runtime", "--no-compile").endswith("63\n")

    code.write_text(source)
    print("PASS runtime-free LLVM executable with transitive static dependencies in Debug and Release")
    left = root / "Left"
    for path in (leaf, left):
        manifest = path / "bolt.proj"
        manifest.write_text(manifest.read_text().replace("static-library", "shared-library"))
    failed = subprocess.run([bolt, "compile", leaf, "--release", *flags], text=True, capture_output=True, timeout=240)
    assert failed.returncode and "Leaf.Hosted" in failed.stderr, failed.stderr
    for profile in ("Debug", "Release"):
        run(*command, "--profile", profile)
        exe = project_artifact(bolt, app, profile)
        libraries = [exe.parent / project_artifact(bolt, dep, profile).name for dep in (leaf, left)]
        assert_runtime_free(exe, *libraries)
        assert libraries[0].name.encode().lower() in libraries[1].read_bytes().lower()
        bundle = root / ("bundle-" + profile)
        bundle.mkdir()
        for path in (exe, *libraries):
            shutil.copy2(path, bundle)
        assert run(bundle / exe.name) == "43\n"
        saved = [bolt, "run", app, "--profile", profile, "--no-runtime", "--no-compile"]
        for change in ("missing", "modified"):
            if change == "missing":
                libraries[0].unlink()
            else:
                with libraries[0].open("ab") as stream:
                    stream.write(b"modified")
            failed = subprocess.run(saved, text=True, capture_output=True, timeout=240)
            assert failed.returncode and libraries[0].name in failed.stderr, failed.stderr
            libraries[0].unlink(missing_ok=True)
            run(*command, "--profile", profile)
            assert run(*saved).endswith("43\n")
    code.write_text(source.replace("Seed(20)", "Seed(30)"))
    assert run(bolt, "run", app, "--release", *flags).endswith("63\n")
    for change in (source.replace("return State.Value;", "return Hosted();"), source + hosted_initializer):
        code.write_text(change)
        failed = subprocess.run([*command, "--release"], text=True, capture_output=True, timeout=240)
        assert failed.returncode and "Leaf.Hosted" in failed.stderr, failed.stderr
        assert run(bolt, "run", app, "--release", "--no-runtime", "--no-compile").endswith("63\n")
    code.write_text(source)
    package = root / "custom-package"
    package.mkdir()
    custom = package / exe.name
    run(*command, "--release", "-o", custom)
    assert run(custom) == "43\n"
    code.write_text(source.replace("return State.Value;", "return State.Value + 1;"))
    run(*command, "--release", "-o", custom)
    assert run(custom) == "45\n"
    print("PASS runtime-free shared graph, relocation, static state, overrides, rebuilds and saved dependencies")

    # Replace just the leaf DSO. Both the existing executable and its intermediate
    # DSO must pick up the independently compiled body and its new string literal.
    leaf_source = 'public static class Message { public: string Text() { return "before"; } }'
    message = project(root, "Message", "shared-library", leaf_source)
    relay = project(root, "Relay", "shared-library",
                    "public static class Relay { public: string Text() { return Message.Text(); } }", ["Message"])
    main = project(root, "Reader", "executable",
                   "public static class Program { public: void Main() { Print(Relay.Text()); } }", ["Relay"])
    run(bolt, "compile", relay, "--release", *flags)
    assert_runtime_free(project_artifact(bolt, relay, "Release"))
    assert run(bolt, "run", main, "--release", *flags).endswith("before\n")
    exe = project_artifact(bolt, main, "Release")
    (message / "src/Main.bolt").write_text(leaf_source.replace("before", "after"))
    run(bolt, "compile", message, "--release", *flags)
    dll = project_artifact(bolt, message, "Release")
    assert_runtime_free(dll)
    shutil.copy2(dll, exe.parent / dll.name)
    assert run(exe) == "after\n"
    print("PASS independently compiled runtime-free shared libraries and transitive replacement")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bolt", type=Path, required=True)
    args = parser.parse_args()
    bolt = args.bolt.resolve()
    capabilities = run(bolt, "backends")
    backends = [name for name, available in (
        ("fast", "Fast: available" in capabilities),
        ("llvm", "LLVM: not available" not in capabilities),
    ) if available]
    assert backends, capabilities
    with tempfile.TemporaryDirectory(prefix="Bolt native libraries ") as directory:
        for backend in backends:
            exercise(bolt, backend, Path(directory) / backend)
        if "llvm" in backends:
            exercise_runtime_free(bolt, Path(directory) / "runtime-free")
