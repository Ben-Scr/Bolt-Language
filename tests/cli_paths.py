#!/usr/bin/env python3
"""Check that output aliases cannot overwrite a .bolt input file."""
import argparse
from pathlib import Path
import subprocess
import os


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bolt", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = work / "Source.bolt"
    content = "static class Program {public:void Main(){Console.WriteLine(1);}}\n"
    source.write_text(content, encoding="utf-8")
    outputs = [source, work / "SOURCE.BOLT"]
    alias = work / "hardlink.cpp"
    if alias.exists():
        alias.unlink()
    try:
        os.link(source, alias)
        outputs.append(alias)
    except OSError:
        print("SKIP hard-link test: filesystem does not permit creating a hard link")
    for target in outputs:
        result = subprocess.run([str(args.bolt.resolve()), "emit", str(source), "-o", str(target)],
                                capture_output=True, text=True, timeout=15)
        if result.returncode == 0 or "overwrite" not in result.stderr:
            raise AssertionError(f"unsafe output accepted: {target}\n{result.stdout}\n{result.stderr}")
        if source.read_text(encoding="utf-8") != content:
            raise AssertionError("input source was modified")
        print("PASS protected output " + target.name)
    print(f"{len(outputs)} output-protection checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
