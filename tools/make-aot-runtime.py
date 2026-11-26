#!/usr/bin/env python3
"""Prepare the GNU/Linux runtime once while building the SDK, using its toolchain."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main(compiler, core, output):
    destination = Path(output).resolve()
    def toolchain_file(name):
        path = Path(subprocess.check_output([compiler, "-print-file-name=" + name], text=True).strip())
        if not path.is_file():
            raise RuntimeError(f"{compiler} cannot locate {name}")
        return str(path.resolve())

    target = subprocess.check_output([compiler, "-dumpmachine"], text=True).strip()
    # RV64GC implements word-sized atomics in hardware. GCC supplies the
    # sub-word operations used by the runtime from libatomic.
    target_arch = target.partition("-")[0]
    extra_runtime = []
    if target_arch == "riscv64":
        extra_runtime.append(toolchain_file("libatomic.a"))
        # glibc implements atexit in this static companion on RV64. Resolve
        # references from libstdc++ here so consumers can also use LLD
        # without installing glibc development archives on the target.
        if "-linux-gnu" in target:
            extra_runtime.append(toolchain_file("libc_nonshared.a"))
    with tempfile.TemporaryDirectory(prefix=".bolt-aot-", dir=destination.parent) as stage:
        partial = Path(stage) / destination.name
        # crtbegin/end own this executable's C++ initializers, destructors and
        # DSO handle. Leave libc, libm, pthread and the ELF loader dynamic.
        subprocess.run([compiler, "-r", "-nostdlib", toolchain_file("crtbeginS.o"),
                        "-Wl,--whole-archive", core, "-Wl,--no-whole-archive",
                        "-Wl,--start-group", toolchain_file("libstdc++.a"),
                        toolchain_file("libgcc.a"), toolchain_file("libgcc_eh.a"), *extra_runtime,
                        "-Wl,--end-group", toolchain_file("crtendS.o"), "-o", str(partial)],
                       check=True)
        partial.replace(destination)


if __name__ == "__main__":
    main(*sys.argv[1:])
