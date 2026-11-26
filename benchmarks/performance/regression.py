#!/usr/bin/env python3
"""Compare same-source runs of suite.py on the same host. Emit performance warnings."""
import argparse
import json
from pathlib import Path
import sys


def check(before, after, profile, threshold):
    for key in ("schema", "metric"):
        if before[key] != after[key]:
            raise ValueError("different benchmark " + key)
    for key in ("platform", "machine", "cpu", "advertised_features"):
        if before["host"].get(key) != after["host"].get(key):
            raise ValueError("different host " + key)
    old_affinity = before["host"].get("cpu_affinity")
    new_affinity = after["host"].get("cpu_affinity")
    if old_affinity is not None and new_affinity is not None and old_affinity != new_affinity:
        raise ValueError("different host cpu_affinity")
    if set(before["cases"]) != set(after["cases"]):
        raise ValueError("different case sets")
    comparison = {}
    for name, previous in before["cases"].items():
        current = after["cases"][name]
        old = previous["profiles"][profile]
        new = current["profiles"][profile]
        if "error" in old or "error" in new:
            raise ValueError(f"{name}/{profile} failed correctness or execution")
        if old["source_sha256"] != new["source_sha256"] or previous["expected_stdout"] != current["expected_stdout"]:
            raise ValueError(f"{name} changed source or expected result")
        if old["command"][1:] != new["command"][1:]:
            raise ValueError(f"{name} changed compile options")
        qualified = (min(old["n"], new["n"]) >= 15 and min(before["warmup"], after["warmup"]) >= 1
                     and min(old["median_ms"], new["median_ms"]) >= max(before["min_duration_ms"], after["min_duration_ms"]))
        if not qualified:
            comparison[name] = {"status": "insufficient-duration-or-samples"}
            continue
        ratio = new["median_ms"] / old["median_ms"]
        comparison[name] = {"status": "warning" if ratio > 1 + threshold / 100 else "ok",
                            "baseline_median_ms": old["median_ms"], "current_median_ms": new["median_ms"],
                            "runtime_change_percent": round((ratio - 1) * 100, 2)}
    return comparison


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("before", type=Path)
    p.add_argument("after", type=Path)
    p.add_argument("--profile", default="release")
    p.add_argument("--threshold-percent", type=float, default=10)
    p.add_argument("--fail-on-warning", action="store_true", help="Fail on a slowdown or an unqualified sample")
    p.add_argument("--json", type=Path)
    args = p.parse_args()
    if args.threshold_percent < 0:
        p.error("threshold must be nonnegative")
    records = [json.loads(f.read_text(encoding="utf-8")) for f in (args.before, args.after)]
    comparison = check(*records, args.profile, args.threshold_percent)
    for case, result in comparison.items():
        delta = result.get("runtime_change_percent")
        suffix = f" ({delta:+.2f}% median time)" if delta is not None else ""
        print(f"{case}: {result['status']}{suffix}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(comparison, indent=2) + "\n", encoding="utf-8")
    return int(args.fail_on_warning and any(item["status"] != "ok" for item in comparison.values()))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, KeyError, ValueError) as error:
        sys.exit(f"Cannot compare reports: {error}")
