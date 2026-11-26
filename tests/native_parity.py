#!/usr/bin/env python3
"""Compare interpreted and native execution, including expected errors.
No third-party Python packages are needed. Each process has a timeout.
"""
from __future__ import annotations
import argparse
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def execute(args: list[str], stdin: str | None = "", timeout: int = 60, *, cwd: Path | None = ROOT) -> subprocess.CompletedProcess[str]:
    with subprocess.Popen(args, stdin=subprocess.PIPE if stdin is not None else None, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding="utf-8", errors="replace", cwd=cwd,
                          start_new_session=os.name == "posix") as child:
        try:
            stdout, stderr = child.communicate(stdin, timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            # A compiler driver can leave a C++ compiler or linker holding our
            # output pipes after the driver exits. End the test's whole group.
            if os.name == "posix":
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            else:
                child.kill()
            child.communicate()
            raise
    return subprocess.CompletedProcess(args, child.returncode, stdout, stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", type=Path, required=True)
    parser.add_argument("--cxx", default="cl.exe" if os.name == "nt" else "c++")
    parser.add_argument("--work", type=Path, default=ROOT / "build" / "parity")
    parser.add_argument("--case", action="append", default=[], help="Run a named case; may be repeated")
    parser.add_argument("--compile-timeout", type=int, default=90, help="Seconds per native compiler invocation")
    parser.add_argument("--discard-artifacts", action="store_true",
                        help="Remove each isolated native build after its case finishes")
    args = parser.parse_args()
    if args.compile_timeout <= 0:
        parser.error("--compile-timeout must be positive")
    bolt = str(args.bolt.resolve())
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    cases = json.loads((ROOT / "tests" / "parity_cases.json").read_text(encoding="utf-8"))
    cases += json.loads((ROOT / "tests" / "feature_cases.json").read_text(encoding="utf-8"))
    cases += json.loads((ROOT / "tests" / "direct_cases.json").read_text(encoding="utf-8"))
    cases += json.loads((ROOT / "tests" / "v5_cases.json").read_text(encoding="utf-8"))
    cases += json.loads((ROOT / "tests" / "arena_cases.json").read_text(encoding="utf-8"))
    selected = set(args.case)
    if selected:
        missing = selected.difference(case["name"] for case in cases)
        if missing:
            parser.error("Unknown case(s): " + ", ".join(sorted(missing)))
        cases = [case for case in cases if case["name"] in selected]
    failed = 0
    for case in cases:
        name = case["name"]
        source = ROOT / case["file"]
        # A space in each output path deliberately exercises process argument handling.
        temporary = tempfile.TemporaryDirectory(prefix=name+"-",dir=work) if args.discard_artifacts else None
        folder = Path(temporary.name) if temporary else work
        target = folder / (name + " native" + (".exe" if os.name == "nt" else ""))
        try:
            interpreted = execute([bolt, "run", str(source), "--runtime"], case.get("stdin", ""))
            compiled = execute([bolt, "build", str(source), "-o", str(target), "--cxx", args.cxx], timeout=args.compile_timeout)
            error = case.get("error")
            if case.get("phase") == "compile":
                if interpreted.returncode == 0 or compiled.returncode == 0:
                    raise AssertionError("invalid source was accepted")
                if error not in interpreted.stderr or error not in compiled.stderr:
                    raise AssertionError(f"missing expected diagnostic {error!r}\n{interpreted.stderr}\n{compiled.stderr}")
            else:
                if compiled.returncode != 0:
                    raise AssertionError("native compilation failed:\n" + compiled.stdout + compiled.stderr)
                native = execute([str(target)], case.get("stdin", ""))
                expected = case.get("stdout", "")
                for mode, result in (("interpreter", interpreted), ("native", native)):
                    # Native regression cases can exercise an explicitly declared
                    # capability outside the portable runtime. Require an exact
                    # capability diagnostic when portable execution is unavailable.
                    unavailable = case.get("interpreter_unavailable")
                    if mode == "interpreter" and unavailable and result.returncode != 0:
                        diagnostic = "--runtime does not support " + unavailable + ";"
                        if result.returncode != 1 or result.stdout or "BLT4000" not in result.stderr or diagnostic not in result.stderr:
                            raise AssertionError(f"unexpected interpreter diagnostic: {result.stderr}")
                        continue
                    if result.stdout != expected:
                        raise AssertionError(f"{mode} output mismatch\nexpected={expected!r}\nactual={result.stdout!r}\n{result.stderr}")
                    if error:
                        if result.returncode == 0 or error not in result.stderr:
                            raise AssertionError(f"{mode}: missing error {error!r}: {result.stderr}")
                    elif result.returncode != case.get("exit", 0):
                        raise AssertionError(f"{mode}: unexpected exit {result.returncode}: {result.stderr}")
            print(f"PASS {name}", flush=True)
        except (AssertionError, OSError, subprocess.TimeoutExpired) as exc:
            failed += 1
            print(f"FAIL {name}: {exc}", file=sys.stderr, flush=True)
        finally:
            if temporary is not None:
                temporary.cleanup()
    print(f"{len(cases)-failed} native-parity cases passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
