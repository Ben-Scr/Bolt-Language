# Arena allocation microbenchmark

`Arena.cpp` compares heap root allocation with the C++ arena support used by emitted programs. It allocates and reads 100,000 objects of 64 bytes, then 5,000 objects of 4 KiB. Each case reports seven-sample median timings after a warm-up. Setup, allocation/reading, cleanup, and final arena disposal are measured separately. Arena cleanup retains backing for reuse; disposal releases it.

Compile from the repository root with a C++20 compiler:

```sh
c++ -std=c++20 -O2 -Iinclude benchmarks/Arena.cpp -o arena-benchmark
./arena-benchmark
```

From a terminal configured for the Microsoft C++ toolchain:

```bat
cl /nologo /std:c++20 /O2 /EHsc /MD /Iinclude benchmarks\Arena.cpp /Fe:arena-benchmark.exe
arena-benchmark.exe
```

The byte columns describe requested heap payload and occupied/retained arena backing. They exclude heap allocator headers, the arena's cleanup records, pointer-tracking vectors, and other process memory. The benchmark counts logical roots, not underlying OS or heap allocator calls. It does not measure nested containers, mixed lifetimes, the interpreter, or native IR backends. Results depend on the machine, compiler, build options, and concurrent activity; they are not a general speedup claim or a test threshold.

## Checked byte-copy benchmark

`MemoryCopy.bolt` times repeated copies of 8, 256 and 4096 bytes. Each iteration changes the source and reads the destination. The timed region includes loop control, checked pointer access and checksum accumulation. Use separate directories for the two executables so each loads its own runtime.

```sh
/path/to/before/bolt compile benchmarks/MemoryCopy.bolt --release --require-native -o before/copy
/path/to/after/bolt compile benchmarks/MemoryCopy.bolt --release --require-native -o after/copy
python3 benchmarks/compare-memory-copy.py before/copy after/copy
```

The driver verifies the six output lines, discards one warm-up per build and alternates execution order for 20 pairs. The per-copy figure is the median elapsed time divided by the copy count. The Linux x86-64 sample on a shared host used a fixed CPU affinity. It compares two Bolt builds. [Measurement and raw samples](results/MemoryCopy-linux-x64.json)

## Buffer growth count

`BufferCapacity.bolt` starts with two elements and counts visible capacity changes across 4,096 single-element `AppendRange` calls. Compile and execute it using each toolchain and its matching runtime. Each run also verifies the final count and values. On the Linux x86-64 direct LLVM release backend the October source baseline changed capacity 4,094 times. The geometric policy changed it 11 times. This count measures changes to the backing capacity. It excludes allocations made by the rest of the process.

```sh
/path/to/before/bolt compile benchmarks/BufferCapacity.bolt --release --require-native -o before/growth
/path/to/after/bolt compile benchmarks/BufferCapacity.bolt --release --require-native -o after/growth
./before/growth
./after/growth
```

## Typed Buffer.ToArray

`BufferToArray.bolt` builds a 131,072-element `Buffer<int>` in advance and performs 256 independent conversions. Each iteration mutates the buffer, copies it to a new array, and includes that array’s last element in the checksum. The executable prints 32640 and the elapsed time in ticks of 100 ns. Compile once with native LLVM Release and deploy the same executable in two directories, each beside its own ABI-matching runtime. Divide ticks by 256 and multiply by 100 to obtain nanoseconds per call.

On a shared Linux x86-64 host the paired medians for the per-element and bulk implementations were 776.9 and 223.2 µs per call. The run used CPU 6, one warm-up pair and 12 alternating measured pairs. The separate long-buffer workload, which includes appending and summing all copied values, took 5.23 and 4.60 seconds per process on CPU 7. Its three alternating measured pairs also include the faster List/Buffer Add dispatch. The small long-workload sample is sensitive to other processes. [Focused timings](results/BufferToArray-linux-x64.json) · [Long workload timings](results/BufferToArray-long-linux-x64.json)
