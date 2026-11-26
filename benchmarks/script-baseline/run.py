"""Reproduce the Windows x64 script baseline from an x64 Visual Studio developer shell."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

from fixtures import CASES, editor_sample

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "examples/FunkeEditorBench"))
from build import dotnet_details, dotnet10_version_key
sys.path.insert(0, str(ROOT / "benchmarks/performance"))
from compare import describe, sha256_file

SEED = 73491
DEBUG_FLAGS = ["--debug", "--backend", "fast", "--require-native"]
RELEASE_FLAGS = ["--release", "--backend", "llvm", "--require-native"]
CSC_FLAGS = ["-nologo", "-nostdlib+", "-target:exe", "-optimize-", "-debug:portable"]


def execute(command, directory, **kwargs):
    start = time.perf_counter_ns()
    p = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=180, **kwargs)
    elapsed = time.perf_counter_ns() - start
    if p.returncode:
        raise RuntimeError(f"{command[0]} exited {p.returncode}\n{p.stdout}\n{p.stderr}")
    return elapsed, p.stdout


def orders(labels, count, rng):
    # Rotate positions within each block and shuffle the rows. Each implementation
    # gets each position equally often across a complete block.
    block = list(labels)
    rounds = []
    while len(rounds) < count:
        rng.shuffle(block)
        rows = [block[i:] + block[:i] for i in range(len(block))]
        rng.shuffle(rows)
        rounds.extend(rows)
    return rounds[:count]


def collect(labels, count, rng, sample):
    raw = {label: [] for label in labels}
    measured = orders(labels, count, rng)
    for i, order in enumerate(orders(labels, 4, rng) + measured):
        for label in order:
            ns = sample(label, i)
            if i >= 4:
                raw[label].append(ns)
    return {"order": measured, "timings": {k: describe(v) for k, v in raw.items()}}


def run_script(command, directory, expected):
    output = directory / "result.bin"
    output.unlink(missing_ok=True)
    ns, _ = execute(command, directory)
    if sha256_file(output) != expected:
        raise ValueError(f"Incorrect result from {command[0]}")
    return ns


def c_build(compiler, source, directory):
    flags = (["/nologo", "/O2", "/DNDEBUG", "/std:c11", str(source), "/Fe:program.exe"]
             if compiler.stem.lower() == "cl" else
             ["-std=c11", "-O3", "-DNDEBUG", str(source), "-o", "program.exe"])
    execute([compiler, *flags], directory)


def csharp_files(directory, references, filename, extra=()):
    (directory / "build.rsp").write_text("\n".join([
        *CSC_FLAGS, f'-out:"{filename}.dll"', *[f'-reference:"{r}"' for r in [*references, *extra]],
        f'"{filename}.cs"']), encoding="utf-8")
    (directory / (filename + ".runtimeconfig.json")).write_text(json.dumps({"runtimeOptions": {
        "tfm": "net10.0", "framework": {"name": "Microsoft.NETCore.App", "version": "10.0.0"}}}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", type=Path, default=ROOT / "bin/bolt.exe")
    parser.add_argument("--clang", type=Path, default=Path(r"C:\Program Files\LLVM\bin\clang.exe"))
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=18)
    args = parser.parse_args()
    if args.rounds < 3:
        parser.error("Specify at least three measured rounds")
    bolt, clang = args.bolt.resolve(strict=True), args.clang.resolve(strict=True)
    if not shutil.which("cl"):
        parser.error("Run from an x64 Visual Studio developer shell")
    cc = {"msvc": Path(shutil.which("cl")), "clang": clang}
    dotnet, sdk, dotroot = dotnet_details("dotnet")
    roslyn = dotroot / "sdk" / sdk / "Roslyn/bincore"
    csc = [dotnet, "exec", roslyn / "csc.dll", "-noconfig", "@build.rsp"]
    pack = max((p for p in (dotroot / "packs/Microsoft.NETCore.App.Ref").glob("10.*")
                if dotnet10_version_key(p.name) and (p / "ref/net10.0").is_dir()),
               key=lambda p: dotnet10_version_key(p.name))
    ref = pack / "ref/net10.0"
    references = sorted(ref.glob("*.dll"))
    native = bolt.with_name("BoltCompiler.dll")
    manifest = json.loads((HERE / "v1.json").read_text())
    rng = random.Random(SEED)
    report = {"baseline": "v1", "utc": datetime.now(timezone.utc).isoformat(), "os": platform.platform(),
              "cpu": platform.processor(), "rounds": args.rounds, "warmups": 4, "seed": SEED,
              "dotnet_sdk": sdk, "reference_pack": pack.name,
              "bolt": json.loads(execute([bolt, "version", "--json"], ROOT)[1]),
              "clang": execute([clang, "--version"], ROOT)[1].splitlines()[0],
              "msvc_path": str(cc["msvc"]), "tool_hashes": {p.name: sha256_file(p) for p in
              [bolt, native, bolt.with_name("BoltRuntime.dll"), roslyn / "csc.dll", *cc.values()]},
              "source_hashes": {p.relative_to(HERE).as_posix(): sha256_file(p)
                                for p in sorted(HERE.rglob("*")) if p.suffix in (".c", ".h", ".cs", ".bolt", ".py") or p.name == "v1.json"},
              "flags": {"bolt_debug": DEBUG_FLAGS, "bolt_release": RELEASE_FLAGS, "csc": CSC_FLAGS,
                        "msvc": "/O2 /DNDEBUG /std:c11", "clang_c": "-O3 -DNDEBUG -std=c11"},
              "cases": {}}
    with tempfile.TemporaryDirectory(prefix="bolt-script-v1-") as temp:
        work = Path(temp)
        host = work / "host"
        host.mkdir()
        shutil.copy(HERE / "WarmHost.cs", host)
        libs = [roslyn / f"Microsoft.CodeAnalysis{s}.dll" for s in ("", ".CSharp")]
        for dll in libs:
            shutil.copy(dll, host)
        csharp_files(host, references, "WarmHost", libs)
        execute(csc, host)
        for name, (generate, before, after) in CASES.items():
            case = work / name
            case.mkdir()
            data, answers = generate()
            hashes = [hashlib.sha256(a).hexdigest() for a in answers]
            if (hashlib.sha256(data).hexdigest() != manifest[name]["input_sha256"] or
                    hashes != manifest[name]["output_sha256"] or hashes[0] == hashes[1]):
                raise ValueError(f"Frozen fixture changed: {name}")
            (case / "input.bin").write_bytes(data)
            for file in ("Program.cs", "Program.bolt"):
                shutil.copy(HERE / name / file, case)
                if (case / file).read_text().count(before) != 1:
                    raise ValueError("Ambiguous edit in " + file)
            result = report["cases"][name] = {"fixture": manifest[name]}
            exe = {}
            for label in ("bolt", *cc):
                dest = case / label
                dest.mkdir()
                if label == "bolt":
                    shutil.copy(case / "Program.bolt", dest)
                    execute([bolt, "compile", "Program.bolt", *RELEASE_FLAGS, "-o", "program.exe"], dest)
                else:
                    c_build(cc[label], HERE / name / "main.c", dest)
                exe[label] = dest / "program.exe"
            result["release"] = collect(exe, args.rounds, rng,
                                        lambda label, _: run_script([exe[label]], case, hashes[0]))
            def fresh(label, _):
                with tempfile.TemporaryDirectory(dir=case) as path:
                    d = Path(path)
                    if label == "bolt":
                        shutil.copy(case / "Program.bolt", d)
                        cmd = [bolt, "compile", "Program.bolt", *DEBUG_FLAGS, "-o", "program.exe"]
                        application = [d / "program.exe"]
                    else:
                        shutil.copy(case / "Program.cs", d)
                        csharp_files(d, references, "Program")
                        cmd = csc
                        application = [dotnet, d / "Program.dll"]
                    ns, _ = execute(cmd, d)
                    run_script(application, case, hashes[0])
                    return ns
            result["debug_cli"] = collect(["bolt", "roslyn"], args.rounds, rng, fresh)
            measured = orders(["bolt", "roslyn"], args.rounds, rng)
            sequence = orders(["bolt", "roslyn"], 4, rng) + measured
            labels = [lang for pair in sequence for lang in pair]
            requests = [f"{lang} {(i+1)%2}" for i, pair in enumerate(sequence) for lang in pair]
            sample, expected = editor_sample(name, data, answers)
            (case / "input.bin").write_bytes(sample)
            warm_hashes = [hashlib.sha256(a).hexdigest() for a in expected]
            if warm_hashes[0] == warm_hashes[1]:
                raise ValueError("The warm fixture must expose the actual edit")
            result["editor_validation"] = {"input_bytes": len(sample), "input_sha256": hashlib.sha256(sample).hexdigest(),
                                           "output_sha256": warm_hashes}
            _, output = execute([dotnet, host / "WarmHost.dll", native, ref, before, after, *warm_hashes],
                                case, input="\n".join(requests) + "\n")
            samples = [round(float(line) * 1e6) for line in output.splitlines()]
            if len(samples) != len(labels):
                raise ValueError("Incomplete warm sample set")
            result["debug_warm"] = {"order": measured, "timings": {
                lang: describe([ns for label, ns in zip(labels[8:], samples[8:]) if label == lang])
                for lang in ("bolt", "roslyn")}}
            print(name, {k: {l: s["median_ms"] for l,s in result[k]["timings"].items()}
                         for k in ("release", "debug_cli", "debug_warm")}, flush=True)
        floor = work / "empty"
        floor.mkdir()
        (floor / "Empty.bolt").write_text("public static class Program { public: void Main() {} }")
        (floor / "empty.c").write_text("int main(void) { return 0; }")
        empty = {}
        for label in ("bolt", *cc):
            dest = floor / label
            dest.mkdir()
            if label == "bolt":
                execute([bolt, "compile", floor / "Empty.bolt", *RELEASE_FLAGS, "-o", "program.exe"], dest)
            else:
                c_build(cc[label], floor / "empty.c", dest)
            empty[label] = dest / "program.exe"
        report["empty_process"] = collect(empty, args.rounds, rng, lambda label, _: execute([empty[label]], floor)[0])
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
