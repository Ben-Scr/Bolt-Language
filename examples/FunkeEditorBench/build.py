#!/usr/bin/env python3
"""Build the headless editor and its managed companion, entirely outside the source tree."""
import argparse
import json
import os
import platform
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent


def dotnet10_version_key(name):
    """Prefer the newest stable .NET 10 build, then the newest prerelease."""
    match = re.fullmatch(r"10\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?", name)
    if not match:
        return ()
    minor, patch, prerelease = match.groups()
    labels = tuple((0, int(part)) if part.isdigit() else (1, part)
                   for part in (prerelease or "").split("."))
    return prerelease is None, int(minor), int(patch), labels


def hostfxr_library(root, filename):
    libraries = [folder / filename for folder in (root / "host/fxr").glob("10.*")
                 if dotnet10_version_key(folder.name) and (folder / filename).is_file()]
    if not libraries:
        raise SystemExit("Cannot find .NET 10 hostfxr beneath the selected SDK root.")
    return max(libraries, key=lambda path: dotnet10_version_key(path.parent.name))


def dotnet_details(command):
    actual = shutil.which(str(command))
    if not actual:
        raise SystemExit("A .NET 10 SDK is required. Pass --dotnet with its executable.")
    tool = Path(actual).resolve()
    sdks = subprocess.check_output([str(tool), "--list-sdks"], text=True).splitlines()
    choices = []
    for line in sdks:
        version, separator, folder = line.strip().partition(" [")
        if separator and folder.endswith("]") and dotnet10_version_key(version):
            choices.append((version, Path(folder[:-1])))
    if not choices:
        raise SystemExit("No .NET 10 SDK found.")
    version, parent = max(choices, key=lambda item: dotnet10_version_key(item[0]))
    return tool, version, parent.parent


def build_managed(command, system, build, output):
    tool, version, dotnet_root = dotnet_details(command)
    rid = {"Windows": "win", "Linux": "linux", "Darwin": "osx", "FreeBSD": "freebsd"}[system]
    arch = {"x86_64":"x64","amd64":"x64","arm64":"arm64","aarch64":"arm64"}[platform.machine().lower()]
    rid += "-" + arch
    host_pack = dotnet_root / "packs" / ("Microsoft.NETCore.App.Host." + rid)
    headers = [header for header in host_pack.glob("10.*/runtimes/" + rid + "/native/hostfxr.h")
               if dotnet10_version_key(header.relative_to(host_pack).parts[0])]
    if not headers:
        raise SystemExit("No .NET 10 native hosting headers for " + rid)
    native_include = max(headers, key=lambda h: dotnet10_version_key(h.relative_to(host_pack).parts[0])).parent
    (build / "global.json").write_text(json.dumps({"sdk": {"version": version, "rollForward": "disable"}}))
    env = dict(os.environ, DOTNET_ROOT=str(dotnet_root), DOTNET_NOLOGO="1", DOTNET_CLI_TELEMETRY_OPTOUT="1")
    subprocess.run([str(tool), "build", str(ROOT/"managed/FunkeEditorBridge.csproj"), "-c", "Release",
                    "-o", str(output), "--configfile", str(ROOT/"fixtures/NuGet.Config"),
                    "-p:BaseIntermediateOutputPath="+str(build/"obj")+os.sep, "-v:q"], cwd=build, env=env, check=True)
    return native_include


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--sdk", type=Path, required=True, help="Bolt SDK root with bin/include")
    cli.add_argument("--output", type=Path, required=True)
    cli.add_argument("--dotnet", default="dotnet")
    cli.add_argument("--bolt-only", action="store_true", help="Build just the Bolt C++ host without a .NET SDK")
    args = cli.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    include = args.sdk.resolve() / "bin" / "include"
    if not (include / "bolt/CompilerHost.h").is_file():
        raise SystemExit("Missing bolt/CompilerHost.h inside the supplied SDK.")
    system = platform.system()
    with tempfile.TemporaryDirectory(prefix="bolt editor build ") as temp:
        build = Path(temp)
        if args.bolt_only:
            native_flags = ["/DBOLT_EDITOR_BOLT_ONLY" if system == "Windows" else "-DBOLT_EDITOR_BOLT_ONLY"]
        else:
            native_include = build_managed(args.dotnet, system, build, output)
            native_flags = [("/I" if system == "Windows" else "-I") + str(native_include)]
        if system == "Windows":
            cmd = ["cl", "/nologo", "/std:c++20", "/EHsc", "/O2", "/W4", "/utf-8",
                   "/I"+str(include), *native_flags, str(ROOT/"editor.cpp"), "/Fe:"+str(output/"BoltEditorSim.exe")]
        else:
            cmd = [os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-Wall", "-Wextra", "-I"+str(include),
                   *native_flags, str(ROOT/"editor.cpp"), "-o", str(output/"BoltEditorSim")]
            if system == "Linux":
                cmd.append("-ldl")
        subprocess.run(cmd, cwd=build, check=True)
    print("Built editor simulator in", output)


if __name__ == "__main__":
    main()
