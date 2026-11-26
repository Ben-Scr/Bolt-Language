"""Integration checks for a staged bolt-watch.py and its matching compiler library."""
import argparse
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("tool", type=Path)
    ap.add_argument("--library", type=Path)
    args = ap.parse_args()
    tool = [sys.executable, str(args.tool.resolve())]
    if args.library:
        tool += ["--library", str(args.library.resolve())]
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        src = root / "src"
        src.mkdir()
        (root / "bolt.proj").write_text('format = 1\n[project]\nname = "WatchTest"\n')
        main_file = src / "Main.bolt"
        main_file.write_text('static class Program { public: int Main(){Print(23); return Helper.Read();} }')
        helper = src / "Helper.bolt"

        def change(value):
            helper.write_text(f'static class Helper {{public: int Read(){{return {value};}}}}', encoding="utf-8")

        def single(mode, events):
            def read():
                for line in proc.stdout:
                    events.put(json.loads(line))
                events.put(None)
            proc = subprocess.Popen([*tool, str(root), *mode], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding="utf-8")
            threading.Thread(target=read, daemon=True).start()
            return proc

        change(17)
        once = subprocess.run([*tool, str(root), "--once"], capture_output=True, encoding="utf-8", timeout=60)
        assert once.returncode == 17 and once.stderr.strip() == '23', (once.returncode, once.stdout, once.stderr)
        assert [json.loads(line)["op"] for line in once.stdout.splitlines()] == ["reload", "run"]

        events = queue.Queue()
        proc = single(["--commands"], events)
        def command(op):
            proc.stdin.write(json.dumps({"op": op}) + "\n")
            proc.stdin.flush()
            event = events.get(timeout=60)
            assert event and event["op"] == op, event
            return event
        try:
            assert not command("run")["ok"]
            checked = command("check")
            assert checked["ok"] and checked["native_codegen_ns"] == 0 and checked["frontend_ns"] > 0
            loaded = command("reload")
            assert loaded["ok"] and loaded["parsed_files"] == 0 and loaded["reused_files"] >= 2
            assert command("run")["exit_code"] == 17
            helper.write_text('static class Helper {')
            assert not command("check")["ok"]
            rejected = command("reload")
            assert not rejected["ok"] and rejected["active_generation"] == 1 and rejected["error"]
            assert command("run")["exit_code"] == 17
            helper.unlink()
            assert not command("reload")["ok"]
            change(29)
            assert command("reload")["active_generation"] == 2
            assert command("run")["exit_code"] == 29
            proc.stdin.write('this line is invalid JSON\n')
            proc.stdin.flush()
            assert not events.get(timeout=60)["ok"]
            assert command("quit")["ok"] and proc.wait(timeout=10) == 0
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

        events = queue.Queue()
        proc = single(["--interval", ".025"], events)
        try:
            assert events.get(timeout=60)["op"] == "reload"
            assert events.get(timeout=60)["exit_code"] == 29
            helper.unlink()
            missing = events.get(timeout=60)
            assert missing["op"] == "reload" and not missing["ok"] and missing["active_generation"] == 1
            change(41)
            assert events.get(timeout=60)["active_generation"] == 2
            assert events.get(timeout=60)["exit_code"] == 41
            old_stamp = helper.stat().st_mtime_ns
            change(42)
            os.utime(helper, ns=(old_stamp, old_stamp))
            # POSIX ctime tracks replacement even if file length and saved mtime are identical.
            if os.name == "posix":
                assert events.get(timeout=60)["ok"]
                assert events.get(timeout=60)["exit_code"] == 42
                proc.send_signal(signal.SIGINT)
                assert proc.wait(timeout=10) == 0
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

        probe = src / "Standalone.bolt"
        probe.write_text('static class Program { public: int Main(){return 11;} }')
        standalone = subprocess.run([*tool, str(probe), "--once"], capture_output=True, text=True, timeout=60)
        assert standalone.returncode == 11, standalone.stdout
    print("bolt-watch passed: single file, project, diagnostics, warm parse reuse, failed reload, "
          "retained old image, added/deleted/modified file, malformed command and clean shutdown")


if __name__ == "__main__":
    main()
