# Bolt compile time and native runtime against C on Windows x64

For 37 native workloads, the sum of Bolt's median compile-and-link times fell **8.3%**, and the sum of its median executable runtimes fell **3.4%**. The two new string and codec workloads accounted for the runtime gain. Across the 35 existing workloads, runtime changed by **+0.14%**. [Raw measurements for all 37 workloads](compiler-runtime-c-windows-x64.json)

| Workloads | Metric | Bolt before (s) | Bolt after (s) | C (s) | Bolt time change | After / C time |
|---|---|---:|---:|---:|---:|---:|
| All 37 | Compile and link | 12.679 | 11.631 | 4.913 | −8.27% | 2.37× |
| All 37 | Execute | 4.954 | 4.786 | 5.386 | −3.39% | 0.89× |
| Original 35 | Compile and link | 12.047 | 11.048 | 4.610 | −8.29% | 2.40× |
| Original 35 | Execute | 4.665 | 4.671 | 5.359 | +0.14% | 0.87× |

Each total adds one unrounded case median from the report for each workload. A long-running workload contributes more to the total. Each build configuration had one warm-up and five measured fresh builds, and each executable had two warm-ups and 15 measured runs. The runner shuffled configuration order within each case. These sums describe this workload mix on one machine. The short builds, process startup and other work on the host limit interpretation of small differences.

Compile medians fell on 36 of 37 workloads in the broad run. The program with the largest runtime gap in favor of C was `float-parse`, at 1,008 ms for Bolt and 155 ms for C. The regular C `dictionary-string-long` took 829 ms compared with 160 ms for Bolt, and the regular C `memory-long` took 734 ms compared with 225 ms for Bolt. The C implementation matters in each comparison.

## Runtime changes

- **Integer list searches.** The standalone runtime loads the element count and copies the needle at its exact width once. It checks groups of eight and resolves matches in index order, including the final partial group. The generic x64 build emitted SSE2 packed compares for 8-, 16- and 32-bit elements and unrolled scalar comparisons for 64-bit elements. Direct runtime-free tests covered duplicates, tails, 64-bit values that differ in one half and an unaligned needle. [Standalone implementation](../../stdlib/System/StandaloneCore.bolt) · [32-bit workload](../performance/cases/list-search.bolt) · [64-bit workload](../performance/cases/list-search-u64.bolt)

- **Hex and Base64.** Encoding fills an array sized for the output and constructs the result string once. Base64 decoding compacts ASCII whitespace in a character array. The codec workload varies length and input across 18 passes, covers all padding lengths and hashes all encoded and decoded bytes. Its median fell from 81.96 to 10.68 ms. The C reference used heap buffers and took 9.13 ms. [Encoding implementation](../../stdlib/System/Text/Encoding.bolt) · [Bolt workload](../performance/cases/text-encoding.bolt) · [C workload](../performance/cases/text-encoding.c)
- **String concatenation.** The native compiler borrows an existing string when evaluating the right operand cannot change it. It snapshots the left operand before an effectful right operand or a formatter call, and transfers a fresh `+=` result to its local destination. The generated `string-concat` loop dropped four `br_text` and two `br_copy` calls per iteration and performs one transfer. The workload constructs 500,000 path names and reads a data-dependent byte from each. Its median fell from 207.38 to 104.35 ms. The C reference writes the same bytes directly into a fixed-size stack buffer and took 17.62 ms. [Bolt workload](../performance/cases/string-concat.bolt) · [C workload](../performance/cases/string-concat.c)

## Workload medians

All entries are milliseconds. Five C/Bolt runtime pairs met the suite's 200 ms minimum median for both executables. These were `arithmetic`, `list-search-long`, `list-search-u64`, `memory-long` and `utf8-index-long`. Treat small differences in the other workloads as descriptive measurements. [Per-run samples, orders, compiler commands and checksums](compiler-runtime-c-windows-x64.json)

| Workload | Build before | Build after | Build C | Run before | Run after | Run C |
|---|---:|---:|---:|---:|---:|---:|
| `arithmetic` | 225.49 | 200.99 | 107.22 | 389.20 | 390.71 | 408.13 |
| `buffer` | 222.01 | 199.21 | 112.76 | 5.15 | 5.11 | 6.05 |
| `buffer-long` | 221.92 | 207.76 | 131.77 | 5.97 | 6.55 | 243.56 |
| `buffer-record-add` | 245.19 | 214.02 | 105.64 | 6.11 | 6.24 | 6.20 |
| `buffer-record-add-long` | 224.72 | 218.49 | 106.44 | 5.08 | 5.13 | 6.78 |
| `buffer-truncate` | 235.83 | 222.65 | 138.50 | 9.09 | 9.28 | 9.10 |
| `buffer-truncate-long` | 242.30 | 223.27 | 139.02 | 134.94 | 133.94 | 134.87 |
| `dictionary-string` | 585.73 | 543.65 | 128.75 | 12.58 | 12.83 | 33.09 |
| `dictionary-string-long` | 629.60 | 566.99 | 159.00 | 160.74 | 160.47 | 828.95 |
| `float-parse` | 301.17 | 275.28 | 115.79 | 1006.59 | 1008.03 | 155.39 |
| `fp-integrate` | 225.48 | 209.32 | 113.86 | 151.81 | 150.76 | 150.38 |
| `indirect-array` | 235.10 | 214.83 | 117.77 | 133.68 | 132.17 | 141.26 |
| `list-edit` | 593.60 | 524.46 | 143.89 | 14.89 | 13.68 | 25.49 |
| `list-edit-long` | 596.40 | 550.08 | 156.88 | 51.06 | 51.55 | 205.85 |
| `list-fill-clear` | 486.75 | 446.24 | 119.83 | 5.03 | 4.75 | 5.60 |
| `list-materialized` | 514.36 | 467.10 | 129.85 | 16.88 | 16.28 | 11.04 |
| `list-search` | 528.20 | 472.15 | 126.09 | 10.60 | 10.71 | 8.89 |
| `list-search-long` | 527.25 | 477.91 | 137.37 | 731.07 | 738.61 | 478.08 |
| `list-search-u64` | 521.05 | 466.52 | 137.07 | 733.17 | 731.04 | 606.24 |
| `memory` | 241.29 | 219.34 | 122.03 | 34.77 | 35.33 | 96.95 |
| `memory-long` | 233.73 | 212.92 | 138.18 | 226.16 | 225.11 | 734.48 |
| `pointers` | 221.79 | 201.02 | 110.98 | 92.03 | 91.05 | 92.93 |
| `queue-worklist` | 597.08 | 569.08 | 145.12 | 23.61 | 23.11 | 19.98 |
| `queue-worklist-long` | 578.09 | 553.82 | 133.02 | 197.28 | 197.58 | 130.25 |
| `string-affixes` | 241.88 | 216.61 | 118.72 | 7.48 | 7.50 | 7.13 |
| `string-concat` | 259.14 | 242.46 | 127.37 | 207.38 | 104.35 | 17.62 |
| `string-equality` | 262.62 | 239.08 | 135.31 | 21.57 | 21.42 | 21.47 |
| `struct-buffer` | 245.16 | 221.57 | 125.19 | 9.89 | 9.04 | 8.83 |
| `struct-copy-small` | 262.84 | 240.54 | 142.43 | 12.15 | 12.09 | 11.98 |
| `struct-copy-wide` | 267.65 | 247.32 | 149.42 | 12.22 | 12.17 | 11.47 |
| `struct-values` | 236.40 | 227.18 | 127.49 | 82.64 | 82.99 | 82.47 |
| `struct-zero` | 297.02 | 272.79 | 150.88 | 140.07 | 142.93 | 137.68 |
| `text-encoding` | 372.69 | 340.26 | 175.50 | 81.96 | 10.68 | 9.13 |
| `utf8-index` | 251.16 | 223.34 | 140.41 | 6.37 | 6.12 | 6.13 |
| `utf8-index-long` | 266.13 | 231.25 | 162.30 | 200.66 | 202.21 | 200.39 |
| `utf8-roundtrip` | 243.41 | 215.54 | 133.31 | 5.48 | 5.40 | 24.09 |
| `utf8-roundtrip-long` | 239.05 | 256.07 | 147.93 | 9.06 | 9.56 | 308.03 |

## Front-end checks

On native hosts, uncached compilations parse 128 embedded standard-library files in up to four batches and merge their syntax trees in source order. Cached hosts and Web builds use serial parsing. Generic-body visibility checks reuse one set of visible names and remove names introduced by each scope when leaving it. The checks covered all 46 visibility and 18 preprocessor CLI groups, plus compiler-host cache reuse, rollback and concurrency. [Standard-library parsing](../../src/StandardLibrary.cpp) · [Generic type resolution](../../src/semantic/TypeResolution.cpp)

Each sample starts a fresh compiler process and measures `bolt check --release --no-timings` through successful exit. The small arithmetic program has 524 source bytes. The generated generic class has 64 fields, 64 methods and 4,096 field additions in 114,867 source bytes. One warm-up pair precedes 15 measured pairs in alternating order. The intervals use 4,000 paired bootstrap resamples. [Front-end measurements](compiler-check-windows-x64.json)

| Input | Bolt before (ms) | Bolt after (ms) | Time saved | 95% interval for time saved |
|---|---:|---:|---:|---:|
| Arithmetic | 93.67 | 70.20 | 25.1% | 21.9–28.6% |
| Generic class | 139.12 | 98.13 | 29.5% | 26.1–29.9% |

## Measurement conditions

The comparison ran on 26 November 2026 on Windows 11 x64, on an AMD Ryzen 9 7900X with the runner and its children restricted to logical CPUs 12–23. Both Bolt SDKs were built in Release with Visual Studio 2022 and MSVC 14.44. Bolt used its LLVM 23.1.2 Release AOT backend for generic x86-64. Clang 23.1.2 compiled the C11 programs with `-O3 -DNDEBUG -ffp-contract=off`. Both used LLD 23.1.2. The SDK and program fingerprints are in the raw measurements. Performance of the JIT, interpreter, Debug edit cycle and other target platforms requires separate measurements.

Full builds start a compiler process in a fresh source and output directory. Timing includes optimization, native code generation, linking and the compiler's normal artifact handling. Builds share the machine's filesystem cache. Each runtime sample starts the finished executable and includes process startup, computation and output. The runner checks its exit code and complete output against the fixture's expected answer. The 502 direct regression checks also passed.

## Reproduction

From the workspace root in an x64 Visual Studio Developer PowerShell, with the two matching SDKs at the paths shown, run

```powershell
$before = 'build/performance-comparison/before/bin/bolt.exe'
$after = 'build/performance-comparison/after/bin/bolt.exe'
$clang = 'C:/Program Files/LLVM/bin/clang.exe'
$env:BOLT_LINKER = 'C:/Program Files/LLVM/bin/lld-link.exe'
$env:BOLT_LLVM_LIBRARY = 'C:/Program Files/LLVM/bin/LLVM-C.dll'
$results = 'benchmarks/results'
(Get-Process -Id $PID).ProcessorAffinity = [IntPtr]0xFFF000
python benchmarks/performance/suite.py --regular --cc $clang --cflag=-ffp-contract=off --cflag=-fuse-ld=lld --before $before --bolt $after --build-runs 5 --warmup 2 --runs 15 --seed 261126 --json "$results/compiler-runtime-c-windows-x64.json"
python benchmarks/performance/compiler_check.py --before $before --bolt $after --warmup 1 --runs 15 --json "$results/compiler-check-windows-x64.json"
```
