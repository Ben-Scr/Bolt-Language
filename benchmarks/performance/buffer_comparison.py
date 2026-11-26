#!/usr/bin/env python3
"""Compare Linux Bolt and GCC on the same Buffer<int> workload."""
import argparse
from collections import Counter
from datetime import datetime, timezone
from itertools import islice
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import tempfile
from types import SimpleNamespace

from compare import describe, executable_path, measure, sha256_file
from suite import build, captured, cpu_info


C_FIXTURES = {"checked_c": "buffer-long-checked", "chunked_c": "buffer-long-chunked"}
PHASES = ("append", "to_array", "sum", "release")
BATCHES = 256
ELEMENTS = 524288
PERIOD = 65536
# XOR with 117 permutes the integers within each period of 65,536.
ELEMENT_SUM = (ELEMENTS // PERIOD) * PERIOD * (PERIOD - 1) // 2
EXPECTED = BATCHES * ELEMENT_SUM + ELEMENTS * sum(i & 31 for i in range(BATCHES))


def balanced_orders(rng, labels):
    """Each compiler occupies each position once in any complete block of three."""
    while True:
        order = list(labels)
        rng.shuffle(order)
        block = [order[i:] + order[:i] for i in range(len(order))]
        rng.shuffle(block)
        yield from block


def check_loader(executable, compiler):
    listing = subprocess.check_output(["ldd", str(executable)], text=True)
    aot = compiler.parent / "BoltAotRuntime.o"
    if aot.is_file():
        if "libBoltRuntime" in listing:
            raise RuntimeError(f"{executable} unexpectedly loads a shared Bolt runtime: {listing}")
        return {"kind": "standalone", "sdk_runtime_sha256": sha256_file(aot), "ldd": listing}
    runtime = (executable.parent / "libBoltRuntime.so").resolve()
    if sha256_file(runtime) != sha256_file(compiler.parent / runtime.name):
        raise RuntimeError(f"{executable} did not receive the selected SDK runtime")
    if f"{runtime.name} => {runtime} (" not in listing:
        raise RuntimeError(f"{executable} loads an unexpected runtime: {listing}")
    return {"kind": "shared", "sdk_runtime_sha256": sha256_file(runtime), "ldd": listing}


def sample(exe, phases, timeout):
    sample = measure(exe, [], timeout)
    fields = sample["stdout"].splitlines()
    if len(fields) != (5 if phases else 1) or int(fields[0]) != EXPECTED:
        raise ValueError(f"unexpected result from {exe}: {fields}")
    result = {"process": sample["time_ns"], "checksum": int(fields[0])}
    if phases:
        spans = [int(value) for value in fields[1:]]
        if min(spans) < 0 or sum(spans) > result["process"]:
            raise ValueError(f"inconsistent monotonic clock intervals from {exe}: {fields}")
        result.update(zip(PHASES, spans))
    return result


def run_case(case, compilers, staging, opts, rng):
    phases = case == "buffer-phases"
    programs, builds = {}, {}
    labels = list(compilers)
    build_opts = SimpleNamespace(codegen_dir=None, cflag=[], boltflag=[], debug_backend="fast",
                                 timeout=opts.timeout)
    for label, compiler in compilers.items():
        profile = "c" if label == "gcc" or label in C_FIXTURES else "release"
        fixture = C_FIXTURES.get(label, case)
        exe, data = build(fixture, profile, compilers["gcc"], compiler,
                          staging / label, build_opts)
        if profile == "release":
            data["runtime_linkage"] = check_loader(exe, compiler)
            objdump = shutil.which("objdump")
            if objdump:
                assembly = captured([objdump, "-d", "--disassemble=bolt_entry", str(exe)],
                                    None, opts.timeout)[1]
                if "<bolt_entry>:" not in assembly:
                    raise RuntimeError(f"cannot locate native entry point in {exe}")
                data["bolt_entry_runtime_call_sites"] = dict(Counter(
                    re.findall(r"\b(?:callq?|bl)\s+[^\n]*<(br_[^@>]+)(?:@plt)?>", assembly)))
        programs[label], builds[label] = exe, data
    warmup_order = list(labels)
    rng.shuffle(warmup_order)
    warmup = {label: sample(programs[label], phases, opts.timeout) for label in warmup_order}
    trials = []
    for order in islice(balanced_orders(rng, labels), opts.runs):
        times = {label: sample(programs[label], phases, opts.timeout) for label in order}
        checksums = {label: values.pop("checksum") for label, values in times.items()}
        trials.append({"order": order, "ns": times, "checksums": checksums})
    metrics = ("process", *PHASES) if phases else ("process",)
    summary = {label: {metric: describe([t["ns"][label][metric] for t in trials])
                       for metric in metrics} for label in labels}
    print(case, {key: values["process"]["median_ms"] for key, values in summary.items()}, flush=True)
    return {"builds": builds, "warmup_order": warmup_order, "warmup": warmup, "raw": trials, "summary": summary}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    comparison = parser.add_mutually_exclusive_group(required=True)
    comparison.add_argument("--before", help="Previous SDK's bin/bolt")
    comparison.add_argument("--checked-c", action="store_true",
                            help="Compare checked GCC, ordinary GCC and Bolt on the uninstrumented long case")
    comparison.add_argument("--chunked-c", action="store_true",
                            help="Compare GCC with explicit capacity chunks, ordinary GCC and Bolt")
    parser.add_argument("--after", required=True, help="New SDK's bin/bolt")
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--cpu", type=int, required=True)
    parser.add_argument("--runs", type=int, default=15)
    parser.add_argument("--seed", type=int, default=491371)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--json", type=Path, required=True)
    opts = parser.parse_args()
    if opts.runs < 1 or opts.timeout < 1:
        parser.error("--runs and --timeout must be positive")
    allowed = sorted(os.sched_getaffinity(0))
    if opts.cpu not in allowed:
        parser.error(f"CPU {opts.cpu} outside process affinity {allowed}")
    os.sched_setaffinity(0, {opts.cpu})
    gcc = executable_path(opts.cc)
    if opts.before:
        reference = {"before": executable_path(opts.before)}
    else:
        reference = {"checked_c" if opts.checked_c else "chunked_c": gcc}
    compilers = {"gcc": gcc, **reference, "after": executable_path(opts.after)}
    toolchains = {}
    for label, path in compilers.items():
        commands = [("--version",)] if label == "gcc" or label in C_FIXTURES else [("version",), ("backends",)]
        toolchains[label] = {"sha256": sha256_file(path),
                             **{cmd[0]: captured([str(path), *cmd], None, opts.timeout)[1]
                                for cmd in commands}}
    report = {"schema": 1, "utc_start": datetime.now(timezone.utc).isoformat(),
              "host": cpu_info(), "cpus_available_before_pin": allowed, "toolchains": toolchains,
              "seed": opts.seed, "warmup_per_binary": 1, "runs": opts.runs,
              "batches": BATCHES, "elements_per_batch": ELEMENTS, "checksum": EXPECTED,
              "cases": {}}
    rng = random.Random(opts.seed)
    with tempfile.TemporaryDirectory(prefix="bolt-buffer-comparison-") as temp:
        cases = ("buffer-long", "buffer-phases") if opts.before else ("buffer-long",)
        for case in cases:
            report["cases"][case] = run_case(case, compilers, Path(temp), opts, rng)
    opts.json.parent.mkdir(parents=True, exist_ok=True)
    opts.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
