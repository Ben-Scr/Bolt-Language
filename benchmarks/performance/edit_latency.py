#!/usr/bin/env python3
"""Time a saved method-body edit through CLI debug build and execution of the new value."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile
import time

from compare import describe, executable_path, sha256_file
from suite import captured, cpu_info

BOLT = """static class Program {
public:
    int Value() { return VALUE; }
    void Main() { Print(Value()); }
}
"""
CSHARP = """using System;
static class Program {
    static int Value() { return VALUE; }
    static void Main() { Console.WriteLine(Value()); }
}
"""


def prepare(language, directory, args):
    directory.mkdir()
    if language in ("bolt", "bolt-before"):
        spec = args.bolt if language == "bolt" else args.baseline_bolt
        tool = executable_path(spec)
        executable = directory / ("probe.exe" if os.name == "nt" else "probe")
        return {
            "source": directory / "Program.bolt", "template": BOLT,
            "build": [str(tool), "compile", "Program.bolt", "--debug", "--backend", args.debug_backend,
                      "--require-native", "-o", executable.name],
            "run": [str(executable)], "version": captured([str(tool), "version"], directory, args.timeout)[1],
            "backends": captured([str(tool), "backends"], directory, args.timeout)[1], "tool_sha256": sha256_file(tool),
        }
    tool = executable_path(args.dotnet)
    version = captured([str(tool), "--version"], directory, args.timeout)[1]
    match = re.fullmatch(r"(\d+)\.\S+", version.splitlines()[-1])
    if not match:
        raise RuntimeError(f"Cannot infer target framework from dotnet --version: {version}")
    framework = "net" + match[1] + ".0"
    (directory / "Probe.csproj").write_text(
        f'<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
        f'<TargetFramework>{framework}</TargetFramework></PropertyGroup></Project>\n', encoding="utf-8")
    (directory / "NuGet.Config").write_text('<configuration><packageSources><clear/></packageSources></configuration>\n',
                                             encoding="utf-8")
    captured([str(tool), "restore", "Probe.csproj", "--configfile", "NuGet.Config"], directory, args.timeout)
    return {
        "source": directory / "Program.cs", "template": CSHARP,
        "build": [str(tool), "build", "Probe.csproj", "-c", "Debug", "--no-restore", "--nologo"],
        "run": [str(tool), str(directory / "bin" / "Debug" / framework / "Probe.dll")],
        "version": version, "target": framework, "tool_sha256": sha256_file(tool),
    }


def time_edit(state, value, timeout):
    source = state["source"]
    source.write_text(state["template"].replace("VALUE", str(value)), encoding="utf-8")
    start = time.perf_counter_ns()
    build_ns, _ = captured(state["build"], source.parent, timeout)
    run_ns, output = captured(state["run"], source.parent, timeout)
    total = time.perf_counter_ns() - start
    if output.strip() != str(value):
        raise RuntimeError(f"edited value {value} expected, observed {output!r}")
    return {"build": build_ns, "launch_and_execute": run_ns, "edit_to_exit": total}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--languages", choices=["bolt", "dotnet"], nargs="+", default=["bolt", "dotnet"])
    parser.add_argument("--debug-backend", choices=["auto", "fast", "llvm"], default="auto")
    parser.add_argument("--bolt", default="bolt")
    parser.add_argument("--baseline-bolt", help="Interleave the previous compiler on the exact same method edits")
    parser.add_argument("--dotnet", default="dotnet")
    parser.add_argument("--runs", type=int, default=15)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--seed", type=int, default=442109)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0 or args.timeout <= 0 or len(set(args.languages)) != len(args.languages):
        parser.error("invalid iterations, timeout, or duplicate language")
    if args.baseline_bolt:
        if "bolt" not in args.languages:
            parser.error("--baseline-bolt requires the bolt language")
        args.languages.append("bolt-before")
    report = {"schema": 1, "utc": datetime.now(timezone.utc).isoformat(), "host": cpu_info(), "seed": args.seed,
              "timeout_s": args.timeout, "warmup": args.warmup, "runs": args.runs,
              "metric": "wall time after closing edited source through new executable process exit",
              "scenario": "warm CLI method edit; default compiler caches; process launch on every edit; .NET restore excluded",
              "languages": {}}
    rng = random.Random(args.seed)
    # Every method body returns a fresh value. Cache hits from a previously sampled source cannot pass.
    values = rng.sample(range(100000, 2000000000), args.warmup + args.runs + 1)
    active = {}
    all_samples = {}
    failed = False
    with tempfile.TemporaryDirectory(prefix="bolt-edits-") as temp:
        for lang in args.languages:
            try:
                state = prepare(lang, Path(temp) / lang, args)
                initial = time_edit(state, values[0], args.timeout)
                report["languages"][lang] = {k: v for k, v in state.items()
                                             if k not in {"source", "template", "build", "run"}}
                report["languages"][lang]["build_command"] = state["build"]
                report["languages"][lang]["run_command"] = state["run"]
                report["languages"][lang]["first_build_and_run_ns"] = initial
                active[lang] = state
                all_samples[lang] = []
            except (RuntimeError, OSError, subprocess.SubprocessError) as error:
                report["languages"][lang] = {"error": str(error)}
                failed = True
        for i, value in enumerate(values[1:]):
            order = list(active)
            rng.shuffle(order)
            for lang in order:
                try:
                    sample = time_edit(active[lang], value, args.timeout)
                    if i >= args.warmup:
                        all_samples[lang].append(sample)
                except (RuntimeError, OSError, subprocess.SubprocessError) as error:
                    report["languages"][lang]["error"] = str(error)
                    del active[lang]
                    failed = True
    for lang in args.languages:
        data = report["languages"][lang]
        if "error" in data:
            print(f"{lang}: {data['error']}", file=sys.stderr)
        else:
            for key in ("build", "launch_and_execute", "edit_to_exit"):
                data[key] = describe([entry[key] for entry in all_samples[lang]])
            print(f"{lang} edit to exit: p50 {data['edit_to_exit']['median_ms']:.2f} ms, "
                  f"p95 {data['edit_to_exit']['p95_ms']:.2f} ms")
    report["measured_values"] = values[1 + args.warmup:]
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
