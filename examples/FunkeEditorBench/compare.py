#!/usr/bin/env python3
"""Replay matched edits in persistent C++ editor processes and verify the first frame after every edit."""
import argparse
import contextlib
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import queue
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time

from build import dotnet_details, hostfxr_library

ROOT = Path(__file__).resolve().parent
FIXTURES = ROOT / "fixtures"
COLUMNS = ["compile_ns", "publish_ns", "first_frame_ns", "compile_to_return_ns", "world_checksum"]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def write_project(path, language, files):
    path.mkdir()
    bolt = language == "bolt"
    ext = "bolt" if bolt else "cs"
    rules = (FIXTURES / ("MovementRules." + ext)).read_text()
    extras = files - 2
    if extras:
        rules = rules.replace("entity % 7ul", "Profile1.Value(entity) % 7ul")
    (path / ("MovementRules." + ext)).write_text(rules, encoding="utf-8")
    for i in range(1, extras + 1):
        body = "v" if i == extras else f"Profile{i+1}.Value(v)"
        method = "public: static" if bolt else "public static"
        text = f"public static class Profile{i} {{ {method} ulong Value(ulong v) {{ return {body} + 1ul; }} }}\n"
        (path / (f"Profile{i}." + ext)).write_text(text, encoding="utf-8")
    if not bolt:
        shutil.copyfile(FIXTURES/"NuGet.Config", path/"NuGet.Config")
        text = (FIXTURES/"Movement.csproj").read_text()
        if language == "csharp-nosymbols":
            text = text.replace("<Nullable>enable</Nullable>", "<Nullable>enable</Nullable><DebugType>none</DebugType><DebugSymbols>false</DebugSymbols>")
        (path/"Movement.csproj").write_text(text, encoding="utf-8")
    return path / ("Movement." + ext), (FIXTURES / ("Movement." + ext + ".in")).read_text()


class Editor:
    def __init__(self, command, env, log, response_timeout=360):
        self.response_timeout = response_timeout
        self.log = log.open("w", encoding="utf-8")
        self.command = [str(value) for value in command]
        try:
            self.process = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                                            text=True, encoding="utf-8", env=env)
        except BaseException:
            self.log.close()
            raise
        self.messages = queue.Queue()

        def read_responses():
            for line in self.process.stdout:
                self.messages.put(line.strip())
            self.messages.put(None)
        self.reader = threading.Thread(target=read_responses, daemon=True)
        self.reader.start()

    def response(self, expected):
        try:
            line = self.messages.get(timeout=self.response_timeout)
        except queue.Empty:
            raise RuntimeError("Editor timed out waiting for " + expected)
        if not line or not line.startswith(expected + " "):
            self.log.flush()
            errors = Path(self.log.name).read_text(encoding="utf-8", errors="replace")
            raise RuntimeError(f"Expected {expected}, got {line!r}\n{errors[-5000:]}")
        return [int(field) for field in line.split()[1:]]

    def send(self, command, expected):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self.response(expected)

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write("quit\n")
                self.process.stdin.flush()
                self.process.wait(timeout=15)
        except (OSError, subprocess.TimeoutExpired):
            self.process.kill()
            self.process.wait()
        finally:
            self.log.close()
            self.process.stdin.close()
            self.process.stdout.close()


def edited_source(template, speed, mode, iteration):
    text = template.replace("SPEED", str(speed))
    if mode == "literal":
        return text
    bolt = "class Program" in text
    call = "MoveRight(entity, step);" if bolt else "moveRight(entity, step);"
    if mode == "body":
        adjustment = ("if (entity % 2ul == 0ul) { step = step + 3ul; }"
                      if iteration % 2 else "if (entity % 2ul != 0ul) { step = step - 1ul; }")
        return text.replace(call, adjustment + "\n            " + call)
    signature = "static ulong" if bolt else "public static ulong"
    if iteration % 2:
        declaration = signature + " Offset(ulong value) { return value + 3ul; }"
        invoke = "Offset(step)"
    else:
        declaration = signature + " Offset(ulong value, ulong amount) { return value - amount; }"
        invoke = "Offset(step, 1ul)"
    head, close = text.rsplit("}", 1)
    return head.replace(call, call.replace("entity, step", "entity, " + invoke)) + "    " + declaration + "\n}" + close


def expected_delta(speed, file_count, mode, iteration):
    checksum = 0
    for entity in range(128):
        value = speed * 16667 // 1_000_000 + (entity + file_count - 2) % 7
        if mode == "declaration" or mode == "body" and entity % 2 != iteration % 2:
            value += 3 if iteration % 2 else -1
        checksum += (entity + 1) * value
    return checksum


def run(args):
    languages = list(dict.fromkeys(args.languages))
    exe = args.bin.resolve() / ("BoltEditorSim.exe" if os.name == "nt" else "BoltEditorSim")
    if not exe.is_file():
        raise SystemExit("Missing native editor executable: " + str(exe))
    libname = {"Windows": "BoltCompiler.dll", "Linux": "libBoltCompiler.so", "Darwin": "libBoltCompiler.dylib", "FreeBSD": "libBoltCompiler.so"}[platform.system()]
    adjacent_lib = args.bin.resolve() / libname
    boltlib = args.bolt_library.resolve() if args.bolt_library else (adjacent_lib if adjacent_lib.is_file() else None)
    if "bolt" in languages and (not boltlib or not boltlib.is_file()):
        raise SystemExit("Pass --bolt-library with the matching BoltCompiler shared library.")
    env = dict(os.environ, DOTNET_NOLOGO="1", DOTNET_CLI_TELEMETRY_OPTOUT="1")
    binaries = {"editor": digest(exe)}
    sdk = None
    if boltlib:
        binaries["BoltCompiler"] = digest(boltlib)
    if any(lang.startswith("csharp") for lang in languages):
        dotnet, sdk, dotnet_root = dotnet_details(args.dotnet)
        fxr_name = {"Windows":"hostfxr.dll","Linux":"libhostfxr.so","Darwin":"libhostfxr.dylib","FreeBSD":"libhostfxr.so"}[platform.system()]
        fxr = hostfxr_library(dotnet_root, fxr_name)
        bridge = args.bin.resolve() / "FunkeEditorBridge.dll"
        env.update(DOTNET_ROOT=str(dotnet_root), PATH=str(dotnet.parent)+os.pathsep+env["PATH"])
        binaries.update(hostfxr=digest(fxr), bridge=digest(bridge), dotnet=digest(dotnet),
                        roslyn_csharp=digest(dotnet_root/"sdk"/sdk/"Roslyn/bincore/Microsoft.CodeAnalysis.CSharp.dll"))
    cpu_model = platform.processor() or platform.machine()
    if Path("/proc/cpuinfo").is_file():
        for line in Path("/proc/cpuinfo").read_text(errors="replace").splitlines():
            if line.startswith("model name"):
                cpu_model = line.partition(":")[2].strip()
                break
    source_files = [ROOT/"editor.cpp", ROOT/"build.py", ROOT/"compare.py", ROOT/"Funke/NativeEngineAPI.hpp"]
    for directory in [FIXTURES, ROOT/"managed"]:
        source_files.extend(f for f in sorted(directory.rglob("*")) if f.is_file()
                            and not {"obj", "bin"}.intersection(f.relative_to(directory).parts))
    results = {"schema":1, "edit":args.edit, "cpu":cpu_model, "utc":time.strftime("%Y-%m-%dT%H:%M:%SZ",time.gmtime()), "platform":platform.platform(),
               "cpu_affinity":sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
               "sdk_dotnet":sdk,"hostfxr_runtime":fxr.parent.name if sdk else None,"funke_commit":"67e570bf2940b975185d0f000aba01aafe6ffbd6",
               "scope":"A source edit is saved before C++ receives reload. A fresh host cold observation includes startup. Warm reload measures compile, publish and first 128-entity frame. CLI transport, checksums and GPU are outside the warm timer.",
               "binary_sha256":binaries,"source_sha256":{f.relative_to(ROOT).as_posix():digest(f) for f in source_files},
               "columns":COLUMNS,"runs":args.runs,"warmup":args.warmup,"seed":args.seed,"scenarios":[]}
    rng = random.Random(args.seed)
    for file_count in args.files:
        with tempfile.TemporaryDirectory(prefix="Bolt Editor Bench ") as temp, contextlib.ExitStack() as stack:
            scenario = {"user_source_files": file_count, "entities":128,"languages":{},"round_orders":[],"speeds":[]}
            results["scenarios"].append(scenario)
            workers = {}
            checksums = {}
            inputs = {}
            launch_order = languages.copy()
            rng.shuffle(launch_order)
            scenario["launch_order"] = launch_order
            for lang in launch_order:
                path = Path(temp)/lang
                src, template = write_project(path,lang,file_count)
                inputs[lang] = src,template
                src.write_text(edited_source(template,4000,args.edit,-1),encoding="utf-8")
                if lang=="bolt":
                    command = [exe,"bolt",boltlib,path]
                else:
                    (path/"global.json").write_text(json.dumps({"sdk":{"version":sdk,"rollForward":"disable"}}))
                    command = [exe,"csharp",fxr,bridge,path/"Movement.csproj",src,path/"bin/Debug/net10.0/Movement.dll",dotnet]
                boot=time.perf_counter_ns()
                worker=Editor(command,env,Path(temp)/(lang+".log"),args.response_timeout)
                stack.callback(worker.close)
                ready = worker.response("READY")[0]
                first = worker.send("reload","FRAME")
                cold = time.perf_counter_ns()-boot
                checksums[lang]=expected_delta(4000,file_count,args.edit,-1)
                if first[-1]!=checksums[lang]:
                    raise RuntimeError(f"{lang}: wrong initial world checksum {first}")
                workers[lang]=worker
                scenario["languages"][lang]={"init_ns":ready,"cold_frame":first,"spawn_to_cold_frame_ns":cold,"warmup":[],"raw":[]}
            orders = list(itertools.permutations(languages))
            order_queue = []
            for i in range(args.runs+args.warmup):
                speed = 8000 + 137*(i+1)
                if not order_queue:
                    order_queue=orders.copy();rng.shuffle(order_queue)
                order = order_queue.pop()
                scenario["round_orders"].append(order)
                scenario["speeds"].append(speed)
                for lang in order:
                    src,template=inputs[lang]
                    src.write_text(edited_source(template,speed,args.edit,i),encoding="utf-8")
                    frame=workers[lang].send("reload","FRAME")
                    checksums[lang]+=expected_delta(speed,file_count,args.edit,i)
                    if frame[-1]!=checksums[lang] or sum(frame[:3])!=frame[3]:
                        raise RuntimeError(f"{lang}: wrong frame result {frame}")
                    scenario["languages"][lang]["warmup" if i<args.warmup else "raw"].append(frame)
            for lang,worker in workers.items():
                src,template=inputs[lang]
                src.write_text(template.replace("SPEED","!!!"),encoding="utf-8")
                rejected=worker.send("reload","REJECT")
                if rejected[-1]!=checksums[lang]:
                    raise RuntimeError("A rejected edit mutated the world")
                retained=worker.send("frame","FRAME")
                checksums[lang]+=expected_delta(speed,file_count,args.edit,i)
                if retained[-1]!=checksums[lang]:
                    raise RuntimeError("Rollback did not retain the previous speed")
                src.write_text(edited_source(template,3400,args.edit,-1),encoding="utf-8")
                repaired=worker.send("reload","FRAME")
                checksums[lang]+=expected_delta(3400,file_count,args.edit,-1)
                if repaired[-1]!=checksums[lang]:
                    raise RuntimeError("Repair failed to activate the new speed")
                sample=scenario["languages"][lang]
                sample.update(rejected=rejected,retained=retained,repaired=repaired)
                summary={}
                for index,column in enumerate(COLUMNS[:4]):
                    values=sorted(row[index]/1e6 for row in sample["raw"])
                    summary[column.replace("_ns","_ms")]={"p50":statistics.median(values),"p95":values[math.ceil(.95*len(values))-1]}
                sample["warm_summary"]=summary
                print(file_count,lang, "cold %.2f ms" % (sample["spawn_to_cold_frame_ns"]/1e6),summary["compile_ms"],summary["compile_to_return_ms"],flush=True)
    args.json.parent.mkdir(parents=True,exist_ok=True)
    args.json.write_text(json.dumps(results,indent=2)+"\n",encoding="utf-8")


if __name__ == "__main__":
    p=argparse.ArgumentParser(description=__doc__)
    cpu = platform.machine().lower()
    arch = {"x86_64":"x64", "amd64":"x64", "arm64":"arm64", "aarch64":"arm64"}.get(cpu, cpu)
    native_dir = ROOT/"bin"/({"Windows":"windows", "Linux":"linux", "Darwin":"macos","FreeBSD":"freebsd"}[platform.system()] + "-" + arch)
    default_bin = native_dir if native_dir.is_dir() else ROOT/"bin"
    p.add_argument("--response-timeout",type=int,default=360,help="Maximum seconds to wait for a compiler or frame response")
    p.add_argument("--bin",type=Path,default=default_bin)
    p.add_argument("--bolt-library",type=Path)
    p.add_argument("--dotnet",default="dotnet")
    p.add_argument("--languages",nargs="+",choices=["bolt","csharp","csharp-nosymbols"],default=["bolt"] if platform.system()=="FreeBSD" else ["bolt","csharp"])
    p.add_argument("--files",type=int,nargs="+",default=[2,32])
    p.add_argument("--edit",choices=["literal","body","declaration"],default="literal",
                   help="Change a numeric constant, a branch and arithmetic, or a called method signature")
    p.add_argument("--warmup",type=int,default=5)
    p.add_argument("--runs",type=int,default=100)
    p.add_argument("--seed",type=int,default=62201)
    p.add_argument("--json",type=Path,required=True)
    options=p.parse_args()
    if options.runs<1 or options.warmup<0 or min(options.files)<2:
        p.error("Expected at least one measurement, two source files and nonnegative warmup.")
    if options.response_timeout<=0:p.error("--response-timeout must be positive")
    run(options)
