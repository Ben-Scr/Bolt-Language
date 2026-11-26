#!/usr/bin/env python3
"""Compare fresh Bolt LLVM and C# Roslyn Release compiler processes on the same programs."""
import argparse
import hashlib
import json
import os
import platform
import random
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

from build import dotnet_details, dotnet10_version_key


def run(command, cwd):
    return subprocess.run([str(x) for x in command], cwd=cwd, check=True,
                          capture_output=True, text=True, timeout=300)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.sha256(stream.read()).hexdigest()


def project(directory, count):
    value = 2
    calls, csharp = [], []
    for i in range(1, count):
        body = f"return (n * 17 + {i}) % 65521;"
        function = f"static int Step(int n) {{ {body} }}"
        (directory / f"Part{i}.bolt").write_text(
            f"public static class Part{i} {{ public: {function} }}\n", encoding="utf-8")
        cs = directory / f"Part{i}.cs"
        cs.write_text(f"public static class Part{i} {{ public {function} }}\n", encoding="utf-8")
        csharp.append(cs)
        call = f"n = Part{i}.Step(n);"
        calls.append(call)
        value = (value * 17 + i) % 65521
    (directory / "Program.bolt").write_text(
        'public static class Program { public: int Main(string[] args) { int n=args.Length;\n'
        + '\n'.join(calls) + '\nPrint(n);return 0;} }\n', encoding="utf-8")
    cs = directory / "Program.cs"
    cs.write_text(
        'public static class Program { public static int Main(string[] args) { int n=args.Length;\n'
        + '\n'.join(calls) + '\nSystem.Console.WriteLine(n);return 0;} }\n', encoding="utf-8")
    return [cs, *csharp], str(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", type=Path, required=True)
    parser.add_argument("--dotnet", default="dotnet")
    parser.add_argument("--files", nargs="+", type=int, default=[2, 32, 128])
    parser.add_argument("--runs", type=int, default=12)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    if min(args.files) < 2 or args.runs < 1 or args.warmup < 0:
        parser.error("Use at least two files and one timed round.")
    bolt = args.bolt.resolve(strict=True)
    dotnet, version, root = dotnet_details(args.dotnet)
    csc = root / "sdk" / version / "Roslyn/bincore/csc.dll"
    packs = root / "packs/Microsoft.NETCore.App.Ref"
    choices = [p for p in packs.glob("10.*") if dotnet10_version_key(p.name)
               and (p / "ref/net10.0/System.Runtime.dll").is_file()]
    if not choices:
        parser.error("The selected .NET installation has no net10.0 reference assemblies.")
    pack = max(choices, key=lambda p: dotnet10_version_key(p.name))
    references = sorted((pack / "ref/net10.0").glob("*.dll"))
    result = {"method": "fresh compiler process and output per build; OS-cache warm; validation outside timed section",
              "outputs": {"bolt": "LLVM native executable", "csharp": "optimized IL assembly"},
              "host": platform.platform(), "dotnet_sdk": version, "reference_pack": pack.name,
              "bolt": json.loads(run([bolt, "version", "--json"], Path.cwd()).stdout),
              "sha256": {"bolt": digest(bolt), "csc": digest(csc), "dotnet": digest(dotnet)},
              "flags": {"bolt": ["--release", "--backend", "llvm", "--require-native"],
                        "csharp": ["-noconfig", "-nostdlib+", "-optimize+", "-debug-", "-target:exe"]},
              "generator_sha256": digest(Path(__file__)), "warmup": args.warmup, "runs": args.runs, "cases": []}
    rng = random.Random(16026)
    for files in args.files:
        rounds = []
        orders = [["bolt", "csharp"] if i % 2 == 0 else ["csharp", "bolt"] for i in range(args.runs)]
        rng.shuffle(orders)
        for i in range(args.warmup + args.runs):
            with tempfile.TemporaryDirectory(prefix="Bolt CLI Release ") as folders:
                work = Path(folders)
                sources, expected = project(work, files)
                native = work / ("program.exe" if os.name == "nt" else "program")
                dll = work / "Program.dll"
                response = work / "csc.rsp"
                response.write_text('\n'.join(["-nologo", "-nostdlib+", "-optimize+", "-debug-",
                    "-target:exe", '-out:"Program.dll"', *[f'-reference:"{p}"' for p in references],
                    *[f'"{p.name}"' for p in sources]]), encoding="utf-8")
                (work / "Program.runtimeconfig.json").write_text(json.dumps({"runtimeOptions": {
                    "tfm": "net10.0", "framework": {"name": "Microsoft.NETCore.App", "version": "10.0.0"}}}))
                commands = {"bolt": [bolt, "compile", work, "--release", "--backend", "llvm", "--require-native", "-o", native],
                            "csharp": [dotnet, "exec", csc, "-noconfig", "@csc.rsp"]}
                order = (["bolt", "csharp"] if i % 2 == 0 else ["csharp", "bolt"]) if i < args.warmup else orders[i - args.warmup]
                timings = {}
                for language in order:
                    start = time.perf_counter_ns()
                    run(commands[language], work)
                    timings[language] = (time.perf_counter_ns() - start) / 1e6
                for cmd in ([native, "one", "two"], [dotnet, dll, "one", "two"]):
                    actual = run(cmd, work).stdout.strip()
                    if actual != expected:
                        raise ValueError(f"Unexpected output {actual!r}, expected {expected!r}")
                rounds.append({"order": order, "warmup": i < args.warmup, "compile_ms": timings})
        median = {lang: statistics.median(r["compile_ms"][lang] for r in rounds if not r["warmup"])
                  for lang in ["bolt", "csharp"]}
        result["cases"].append({"files": files, "expected_output": expected, "median_ms": median, "rounds": rounds})
        args.json.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(files, median, flush=True)


if __name__ == "__main__":
    main()
