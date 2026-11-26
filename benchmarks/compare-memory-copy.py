"""Compare two compiled Release versions of MemoryCopy.bolt on one host."""

import argparse
import json
import platform
import statistics
import subprocess
from pathlib import Path

CASES = [(8, 524288), (256, 524288), (4096, 131072)]


def measure(binary):
    output = subprocess.run([str(binary)], check=True, text=True, capture_output=True,
                            timeout=60).stdout.splitlines()
    if len(output) != 2 * len(CASES):
        raise ValueError(f"Unexpected output from {binary}: {output!r}")
    samples = []
    for i, (_, count) in enumerate(CASES):
        checksum = int(output[2 * i])
        ticks = int(output[2 * i + 1])
        if checksum != (count // 128) * sum(range(128)) or ticks <= 0:
            raise ValueError(f"Invalid measurement from {binary} for case {i}: {output!r}")
        samples.append(ticks)
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--pairs", type=int, default=20)
    args = parser.parse_args()
    if args.pairs < 2:
        parser.error("--pairs must be at least 2")
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    for binary in binaries.values():
        measure(binary)
    raw = {name: [] for name in binaries}
    for i in range(args.pairs):
        order = ("before", "after") if i % 2 == 0 else ("after", "before")
        for name in order:
            raw[name].append(measure(binaries[name]))
    results = []
    for i, (width, count) in enumerate(CASES):
        case = {"bytes": width, "copies": count}
        for name in binaries:
            ticks = [row[i] for row in raw[name]]
            case[name + "_median_ns_per_copy"] = statistics.median(ticks) * 100 / count
        case["before_over_after"] = case["before_median_ns_per_copy"] / case["after_median_ns_per_copy"]
        results.append(case)
    print(json.dumps({"host": platform.platform(), "pairs": args.pairs,
                      "stopwatch_tick_ns": 100, "results": results,
                      "raw_ticks_by_run": raw}, indent=2))


if __name__ == "__main__":
    main()
