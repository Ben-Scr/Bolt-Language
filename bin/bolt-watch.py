#!/usr/bin/env python3
"""Keep a native Bolt Debug compiler alive while editing a script or project."""
import argparse
import ctypes as C
import json
import math
import os
from pathlib import Path
import sys
import time
import tomllib


class Stats(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in
                ("parsed_files", "reused_files", "frontend_ns", "native_codegen_ns")]

    def values(self):
        return {name: getattr(self, name) for name, _ in self._fields_}


class Host:
    def __init__(self, library, path):
        self.lib = C.CDLL(str(library))
        self.path = str(path).encode("utf-8")
        self.compiler = C.c_void_p()
        self.generation = 0
        handle, size, text = C.c_void_p, C.c_size_t, C.c_char_p
        signatures = {
            "bolt_compiler_host_abi": (C.c_uint32,),
            "bolt_compiler_create": (C.c_int, C.c_uint32, C.c_int, handle, size, C.POINTER(handle), text, size),
            "bolt_compiler_destroy": (None, handle),
            "bolt_compiler_compile_project": (C.c_int, handle, text, C.POINTER(handle), text, size),
            "bolt_compiler_check_project": (C.c_int, handle, text, C.POINTER(Stats), text, size),
            "bolt_module_stats": (C.c_int, handle, C.POINTER(Stats)),
            "bolt_module_release": (None, handle),
            "bolt_compiler_load": (C.c_int, handle, handle, text, size),
            "bolt_compiler_run": (C.c_int, handle, C.POINTER(C.c_int32), text, size),
        }
        for name, (result, *args) in signatures.items():
            fn = getattr(self.lib, name)
            fn.argtypes, fn.restype = args, result
        if self.lib.bolt_compiler_host_abi() != 1:
            raise RuntimeError("bolt-watch requires Bolt compiler host ABI 1")
        self.call(self.lib.bolt_compiler_create, 1, 0, None, 0, C.byref(self.compiler))

    @staticmethod
    def call(fn, *args):
        error = C.create_string_buffer(16384)
        status = fn(*args, error, len(error))
        if status:
            raise RuntimeError(error.value.decode("utf-8", errors="replace") or f"host status {status}")

    def command(self, op):
        start = time.perf_counter_ns()
        reply = {"op": op, "ok": True}
        try:
            if op == "check":
                stats = Stats()
                self.call(self.lib.bolt_compiler_check_project, self.compiler, self.path, C.byref(stats))
                reply.update(stats.values())
            elif op == "reload":
                module = C.c_void_p()
                self.call(self.lib.bolt_compiler_compile_project, self.compiler, self.path, C.byref(module))
                try:
                    stats = Stats()
                    if self.lib.bolt_module_stats(module, C.byref(stats)):
                        raise RuntimeError("could not read compilation statistics")
                    reply.update(stats.values())
                    load = time.perf_counter_ns()
                    self.call(self.lib.bolt_compiler_load, self.compiler, module)
                    self.generation += 1
                    reply["load_ns"] = time.perf_counter_ns() - load
                finally:
                    self.lib.bolt_module_release(module)
            elif op == "run":
                code = C.c_int32()
                self.call(self.lib.bolt_compiler_run, self.compiler, C.byref(code))
                reply["exit_code"] = code.value
            elif op != "quit":
                raise ValueError("expected check, reload, run, or quit")
        except (ValueError, RuntimeError) as error:
            reply.update(ok=False, error=str(error))
        reply.update(active_generation=self.generation, wall_ns=time.perf_counter_ns() - start)
        return reply

    def close(self):
        self.lib.bolt_compiler_destroy(self.compiler)


EXCLUDED = {".idea", ".git", ".vs", ".bolt", "bin", "obj", "build", "out", "vendor", "node_modules"}
CONFIGS = {"bolt.proj", "bolt.conf", "bolt.lock"}


def watch_paths(target, extra):
    """Track declared source roots, config paths and transitive project dependencies."""
    paths = {target}
    roots = set(extra)
    manifest = None
    if target.name == "bolt.proj":
        manifest = target
    elif target.is_dir():
        manifest = next((p / "bolt.proj" for p in (target, *target.parents) if (p / "bolt.proj").is_file()), None)
        if manifest is None:
            roots.add(target)
    elif target.suffix == ".bolt":
        roots.add(target.parent)
    pending, seen = [manifest] if manifest else [], set()
    while pending:
        file = pending.pop()
        if file in seen:
            continue
        seen.add(file)
        paths.update(file.parent / name for name in CONFIGS)
        try:
            with file.open("rb") as stream:
                config = tomllib.load(stream)
            for root in config.get("sources", {}).get("roots", ["src"]):
                roots.add((file.parent / root).resolve())
            for dep in config.get("dependencies", {}).values():
                path = (file.parent / dep["path"]).resolve()
                pending.append(path if path.name == "bolt.proj" else path / "bolt.proj")
        except (OSError, ValueError, TypeError, KeyError, AttributeError):
            # Keep watching the broken manifest and root so the next edit can repair it.
            roots.add(file.parent)
    for root in roots:
        paths.add(root)
        for base, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d.lower() not in EXCLUDED and not (Path(base) / d).is_symlink()]
            paths.update(Path(base) / name for name in files if Path(name).suffix.lower() == ".bolt" or name in CONFIGS)
    return paths


def snapshot(target, extra):
    result = {}
    for path in watch_paths(target, extra):
        try:
            s = path.stat()
            if path.is_dir():
                # Enumeration above detects source additions. Ignore unrelated directory writes.
                result[path] = "directory"
            else:
                result[path] = (s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
        except OSError:
            result[path] = None
    return result


def default_library():
    folder = Path(__file__).resolve().parent
    filename = {"win32": "BoltCompiler.dll", "darwin": "libBoltCompiler.dylib"}.get(sys.platform, "libBoltCompiler.so")
    for base in (folder, folder.parent / "libexec", folder.parent / "lib"):
        if (base / filename).is_file():
            return base / filename
    return folder / filename


def redirect_descriptor(target, source):
    os.dup2(source, target)
    if sys.platform == "win32":
        # Native console code can bypass the CRT and call GetStdHandle directly.
        import msvcrt
        set_std = C.windll.kernel32.SetStdHandle
        set_std.argtypes, set_std.restype = [C.c_uint32, C.c_void_p], C.c_int
        kind = {0: -10, 1: -11}[target]
        if not set_std(kind & 0xffffffff, msvcrt.get_osfhandle(target)):
            raise C.WinError()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("path", type=Path, help="single .bolt file, project folder, or bolt.proj")
    p.add_argument("--library", type=Path, default=default_library())
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--once", action="store_true", help="compile and run once")
    mode.add_argument("--commands", action="store_true", help="read JSONL requests from stdin; emit one JSONL response per line")
    p.add_argument("--interval", type=float, default=0.1, help="seconds between directory scans")
    p.add_argument("--watch-root", type=Path, action="append", default=[], help="additional source directory to observe")
    args = p.parse_args()
    if not math.isfinite(args.interval) or args.interval <= 0:
        p.error("--interval must be a positive, finite number of seconds")
    path = args.path.resolve()
    extra = [root.resolve() for root in args.watch_root]
    requests = None
    with os.fdopen(os.dup(sys.stdout.fileno()), "w", encoding="utf-8", buffering=1) as events:
        # Script writes must not corrupt the event stream or consume the next JSON command.
        sys.stdout.flush()
        redirect_descriptor(sys.stdout.fileno(), sys.stderr.fileno())
        if args.commands:
            requests = os.fdopen(os.dup(sys.stdin.fileno()), "r", encoding="utf-8", errors="replace")
            with open(os.devnull, "rb") as empty:
                redirect_descriptor(sys.stdin.fileno(), empty.fileno())
        def send(record):
            print(json.dumps(record, ensure_ascii=True), file=events, flush=True)

        host = None
        try:
            host = Host(args.library.resolve(), path)
            if requests:
                for line in requests:
                    try:
                        request = json.loads(line)
                        if not isinstance(request, dict) or not isinstance(request.get("op"), str):
                            raise ValueError("expected a JSON object with a string op")
                        result = host.command(request["op"])
                    except ValueError as error:
                        result = {"ok": False, "error": str(error), "active_generation": host.generation}
                    send(result)
                    if result.get("op") == "quit":
                        break
                return 0
            previous = snapshot(path, extra) if not args.once else {}
            while True:
                loaded = host.command("reload")
                send(loaded)
                if loaded["ok"]:
                    run = host.command("run")
                    send(run)
                    if args.once:
                        return run.get("exit_code", 1)
                elif args.once:
                    return 1
                # A change made while compiling is detected on the next comparison.
                while True:
                    time.sleep(args.interval)
                    candidate = snapshot(path, extra)
                    if candidate != previous:
                        time.sleep(args.interval)
                        if candidate == snapshot(path, extra):
                            previous = candidate
                            break
        except BrokenPipeError:
            return 0
        except (OSError, RuntimeError, AttributeError) as error:
            send({"ok": False, "error": str(error)})
            return 1
        except KeyboardInterrupt:
            return 0
        finally:
            if host:
                host.close()
            if requests:
                requests.close()


if __name__ == "__main__":
    sys.exit(main())
