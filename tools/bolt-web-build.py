#!/usr/bin/env python3
"""Compile a Bolt entry point or project to a browser/Node ES module."""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
from urllib.parse import quote


def wasm_field(data):
    """Encode one length-prefixed WebAssembly byte sequence."""
    size = len(data)
    prefix = bytearray()
    while size >= 128:
        prefix.append((size & 127) | 128)
        size >>= 7
    prefix.append(size)
    return prefix + data


def publish_source_map_url(wasm, url):
    # WebAssembly source maps use instruction offsets in the original binary.
    data = wasm.read_bytes()
    name = wasm_field(b"sourceMappingURL")
    offset = 8
    seen_code = False
    while offset < len(data):
        start = offset
        section = data[offset]
        offset += 1
        size = 0
        for shift in range(0, 35, 7):
            byte = data[offset]
            offset += 1
            size |= (byte & 127) << shift
            if byte < 128:
                break
        end = offset + size
        if section == 0 and data.startswith(name, offset, end):
            replacement = bytes([0]) + wasm_field(name + wasm_field(url.encode("utf-8")))
            if seen_code:
                wasm.write_bytes(data[:start] + replacement + data[end:])
            else:
                # Preserve all offsets after an early URL section with an empty
                # custom section of equal size, then append the corrected URL.
                wasm.write_bytes(data[:offset] + bytes(size) + data[end:] + replacement)
            return
        if section == 10:
            seen_code = True
        offset = end
    raise ValueError("Emscripten did not emit a WebAssembly source map URL")


def map_debug_sources(generated):
    """Give source files reversible ASCII labels while the Wasm toolchain builds its line table."""
    labels = {}
    originals = {}

    def label(match):
        name = json.loads(match[2])
        path = Path(name)
        if not path.is_absolute() or path.suffix != ".bolt":
            return match[0]
        if name not in labels:
            alias = f"/__bolt_web_sources__/{len(labels)}.bolt"
            labels[name] = alias
            originals[alias] = path
        return match[1] + json.dumps(labels[name])

    # Bolt emits quoted UTF-8 paths. Some llvm-dwarfdump/wasm-sourcemap
    # combinations turn each non-ASCII byte into a slash and octal digits.
    # Mapping at the producer avoids guessing which on-disk name they meant.
    text = generated.read_text(encoding="utf-8")
    rewritten = re.sub(r'^(#line [0-9]+ )("(?:[^"\\]|\\.)*")$', label, text, flags=re.MULTILINE)
    if labels:
        generated.write_text(rewritten, encoding="utf-8")
    return originals


def publishable_source_map(staged, output, entry, originals):
    """Keep paths valid after staging and include the project's Bolt sources for remote debugging."""
    data = json.loads(staged.read_text(encoding="utf-8"))
    root = entry if entry.is_dir() else entry.parent
    sources, contents = [], []
    for name in data["sources"]:
        source = (staged.parent / name).resolve()
        # The synthetic label starts at the root. On Windows the output can
        # be on a different drive from the caller's working directory.
        label = "/" + source.relative_to(source.anchor).as_posix()
        source = originals.get(label, source)
        try:
            relative = os.path.relpath(source, output.parent)
            sources.append(quote(Path(relative).as_posix(), safe="/"))
        except ValueError:  # Source and destination are on different Windows drives.
            sources.append(source.as_uri())
        if source.suffix == ".bolt" and root in source.parents and source.is_file():
            contents.append(source.read_text(encoding="utf-8"))
        else:
            contents.append(None)
    data["x_bolt_wasm"] = output.with_suffix("").name
    data["sources"] = sources
    data["sourcesContent"] = contents
    staged.write_text(json.dumps(data, separators=(",", ":")), encoding="utf-8")


def generated_map(path, wasm):
    if not path.is_file() or path.is_symlink():
        return False
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeError, ValueError, OSError):
        return False
    return isinstance(data, dict) and data.get("x_bolt_wasm") == wasm.name and data.get("version") == 3


def build(args):
    sdk = args.sdk.resolve()
    header = sdk / "include/bolt/Version.hpp"
    archive = sdk / "lib/libBoltInterpreter.a"
    if not header.is_file():
        raise ValueError(f"Expected a Web SDK with headers under {sdk / 'include/bolt'}")
    output = args.output.resolve()
    if output.suffix != ".mjs":
        raise ValueError("Use a .mjs output name. Its .wasm companion will be written beside it.")
    entry = args.source.resolve(strict=True)
    wasm = output.with_suffix(".wasm")
    source_map = wasm.with_suffix(".wasm.map")
    targets = (source_map, wasm, output) if args.debug else (wasm, output)
    for target in targets:
        if target.exists() and not target.is_file():
            raise ValueError(f"Output must be a file: {target}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".bolt-web-", dir=output.parent) as folder:
        stage = Path(folder)
        generated = stage / "program.cpp"
        subprocess.run([args.bolt, "emit", str(entry), "--target", "web",
                        "--debug" if args.debug else "--release", "-o", str(generated)], check=True)
        # The emitter adds this include only when the program uses the Bolt thread kernel.
        threaded = '#include <bolt/detail/HostedIntrinsics.hpp>' in generated.read_text(encoding="utf-8")
        thread_flags = []
        runtime_inputs = [str(archive)] if archive.is_file() else []
        if threaded:
            # callMain is synchronous. Preload workers before resolving the ES module
            # and let pthread_create report EAGAIN if no worker is available.
            thread_flags = ["-pthread", f"-sPTHREAD_POOL_SIZE={args.workers}", "-sPTHREAD_POOL_SIZE_STRICT=2"]
            threaded_archive = sdk / "lib/libBoltInterpreter-mt.a"
            if threaded_archive.is_file():
                runtime_inputs = [str(threaded_archive)]
        if not runtime_inputs or (threaded and not threaded_archive.is_file()):
            # AOT uses only the standard diagnostics unless a program explicitly
            # embeds the interpreter. Compile these with the requested memory ABI.
            diagnostics = stage / "diagnostics.cpp"
            diagnostics.write_text("#include <bolt/detail/DiagnosticsImplementation.inc>\n", encoding="utf-8")
            runtime_inputs.insert(0, str(diagnostics))
        originals = map_debug_sources(generated) if args.debug else {}
        profile_flags = ["-O0", "-gsource-map", "--source-map-base", "./"] if args.debug else ["-O3"]
        command = [args.emxx, "-std=c++20", "-fexceptions", "-sMEMORY64=2",
                   "-I", str(sdk / "include"), str(generated), *runtime_inputs, *thread_flags,
                   *profile_flags, "-sMODULARIZE=1", "-sEXPORT_ES6=1",
                   "-sENVIRONMENT=web,worker,node", "-sALLOW_MEMORY_GROWTH=1", "-sSTACK_SIZE=4194304",
                   "-sINVOKE_RUN=0", "-sEXPORTED_RUNTIME_METHODS=callMain",
                   "-o", str(stage / output.name)]
        encoded = quote(wasm.name, safe="")
        if encoded != wasm.name:
            # Emscripten's default loader passes an unescaped filename to URL.
            # Retain the original filename for applications with a locateFile hook.
            loader = stage / "asset-url.js"
            loader.write_text(
                "if (!Module['locateFile']) {\n"
                "  Module['locateFile'] = (path, prefix) => path === " + json.dumps(wasm.name) + "\n"
                "    ? new URL(" + json.dumps("./" + encoded) + ", import.meta.url).href : prefix + path;\n"
                "}\n", encoding="utf-8")
            command.extend(["--pre-js", str(loader)])
        subprocess.run(command, check=True)
        # Some Emscripten versions emit a separate worker bootstrap.
        for worker in stage.iterdir():
            if worker.is_file() and worker.name.startswith(output.stem + ".worker."):
                targets = (output.parent / worker.name, *targets)
        for target in targets:
            if not (stage / target.name).is_file():
                raise ValueError(f"Emscripten did not produce {target.name}")
        if args.debug:
            if encoded != wasm.name:
                publish_source_map_url(stage / wasm.name, "./" + encoded + ".map")
            publishable_source_map(stage / source_map.name, source_map, entry, originals)
        # Restore the previous export if publishing a companion file fails.
        backups = {}
        replaced = []
        try:
            for target in targets:
                if target.exists():
                    backup = stage / (target.name + ".previous")
                    shutil.copy2(target, backup)
                    backups[target] = backup
                os.replace(stage / target.name, target)
                replaced.append(target)
            if not args.debug and generated_map(source_map, wasm):
                source_map.unlink()
        except OSError:
            for target in reversed(replaced):
                if target in backups:
                    os.replace(backups[target], target)
                else:
                    target.unlink(missing_ok=True)
            raise
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help=".bolt entry point or project directory")
    parser.add_argument("-o", "--output", required=True, type=Path, help="destination ES module (.mjs)")
    parser.add_argument("--sdk", type=Path, default=Path(__file__).resolve().parent,
                        help="extracted Bolt Web SDK directory")
    parser.add_argument("--bolt", default=os.environ.get("BOLT_COMPILER", "bolt"), help="desktop Bolt compiler")
    parser.add_argument("--emxx", default=os.environ.get("EMXX", "em++"), help="Emscripten C++ driver")
    parser.add_argument("--workers", type=int, default=8, help="preloaded workers for threaded Web programs (default 8)")
    parser.add_argument("--debug", action="store_true", help="emit unoptimized WebAssembly and its .wasm.map for source debugging")
    args = parser.parse_args()
    if args.workers < 1:
        parser.error("--workers must be positive")
    try:
        print(build(args))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"bolt-web-build: {error}\n")


if __name__ == "__main__":
    main()
