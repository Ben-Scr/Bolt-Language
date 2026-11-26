# Toolchain development and troubleshooting

## Build the compiler

Use CMake, a C++20 compiler and a native build tool for your host. Install Python for standard-library embedding and Python-based tests. A compatible shared LLVM library and target linker enable LLVM AOT/JIT workflows. From a complete Bolt source checkout, configure an out-of-source build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On a single-configuration generator, `CMAKE_BUILD_TYPE` selects the configuration. With Visual Studio or another multi-configuration generator, `--config Release` selects it at build time. The generated compiler normally lives under `build/bin` or `build/bin/Release`. Run that **specific executable** when checking a compiler change. An installed `bolt` on the path may still use a different parser or embedded standard library.

For a compiler-only build, configure with `-DBOLT_BUILD_TESTS=OFF -DBOLT_BUILD_EMBEDDING=OFF` and build the `Bolt` target. Keep the runtime artifacts it builds with the resulting compiler. Add `-DBOLT_SANITIZE=ON` to a suitable Clang/GCC build to enable address and undefined-behavior sanitizers. [CMake targets and options](../CMakeLists.txt)

## Find the implementation

| Area | Starting points |
|---|---|
| Tokens, parsing and conditional source | [Lexer](../src/Lexer.cpp), [parser](../src/Parser.cpp), [preprocessor](../src/Preprocessor.cpp) |
| Types, name resolution and diagnostics | [Semantic analysis](../src/Semantic.cpp), [type helpers](../include/bolt/Types.hpp), [diagnostic contracts](../include/bolt/Diagnostics.hpp) |
| Projects, command dispatch and artifacts | [Project loader](../src/Project.cpp), [CLI](../src/Main.cpp), [project helpers](../src/cli) |
| Execution | [Interpreter](../src/Interpreter.cpp), [typed intermediate representation](../src/native/IR.cpp), [native lowering](../src/native/Lower.cpp), [LLVM backend](../src/native/LLVM.cpp), [C++ emitter](../src/Emitter.cpp) |
| Library and runtime | [Bolt library source](../stdlib), [embedding step](../tools/embed-stdlib.py), [native ABI](../include/bolt/native/ABI.hpp) |
| Hosts and editors | [Compiler host](../src/CompilerHost.cpp), [interpreter host](../src/InterpreterHost.cpp), [language server](../src/tooling/LanguageServer.cpp) |

CMake regenerates the embedded library from its source inputs. Rebuild the compiler after a library edit. Editing a readable SDK copy of a `.bolt` library file does not update the code embedded in an already-built compiler.

## Select a test

Use `ctest --test-dir build -C Release -N` to see which tests were registered for your host and prerequisites. An individual test is useful when iterating, for example `ctest --test-dir build -C Release -R CompilerHost --output-on-failure`. For an existing Windows desktop SDK, the following commands exercise the actual binaries directly from the source directory.

```powershell
$env:Path = (Resolve-Path .\bin).Path + ';' + $env:Path
python tests/lsp_protocol.py --bolt bin/bolt.exe
python tests/integer_casts.py bin/bolt.exe
python tests/compiler_host.py bin/BoltCompiler.dll
```

The editor test starts a real language-server process. The numeric tests compare computed boundary results in native builds. The host test exercises module ownership, replacement, rollback and concurrent use. Backend parity tests in `tests` compare execution paths. Cross-platform tests have their own SDK, architecture or emulator prerequisites. [Test directory](../tests)

## Measure a change

Choose the measurement that corresponds to the user's wait. For a game editor that may be time from editing source to the first frame that uses it. For an exported program it may be the full process duration or just the loop processing the data. Record build flags, target and whether a process or compiler was already warm. Validate program output on every comparison.

- [Performance suite](../benchmarks/performance/README.md) covers runtime kernels, startup, memory operations and edit latency.
- [Script baseline](../benchmarks/script-baseline/README.md) and [holdout workloads](../benchmarks/script-holdout/README.md) compare realistic scripts and document input data and measurement boundaries.
- [Funke editor simulator](../examples/FunkeEditorBench/README.md) measures compilation and activation in a host. [Recorded results](../benchmarks/results) retain the underlying data.

## Troubleshooting

| Symptom | What to check |
|---|---|
| The terminal and editor accept different syntax | Compare the executable path from `bolt version --json` with **Bolt: Show Toolchain Info**. Select the intended compiler in the editor, then restart its language server. If working on source features, use the compiler you built. |
| `bolt init` refuses a destination | Choose a new directory. To work in an existing project, open its manifest and run `bolt check .` there. |
| A name in another file is unresolved | Run `bolt project info . --format json`, check `all_sources`, imports, visibility and exact path spelling. Point the compiler at the project directory. |
| A generic call or import checks but the native build fails | Run the same build with `--require-native` to get the backend constraint. Provide a compatible implementation or build via `--backend cpp` with a configured C++ toolchain. |
| A fast debug build requests LLVM | Check the profile's `debug_info` setting. Where Fast cannot produce the requested symbols, choose LLVM or request `--backend fast --no-debug-info` for a build that can omit them. |
| LLVM, linker or target startup objects cannot be found | Run `bolt backends`, inspect `--verbose` output and verify `--llvm`, `--linker`, `--runtime-dir` and `--sysroot` for the chosen target. |
| Runtime ABI mismatch or a missing DLL | Use the compiler, libraries, headers and runtime from one matching build and architecture. Rebuild the application. Check executable-adjacent libraries and the OS library search path. |
| The program runs old code | `--no-compile` selects a saved executable. Rebuild the intended profile, verify its output path and look at `BOLT_BUILD_ROOT` if set. |
| File I/O uses an unexpected directory or input is missing | Inspect `run.working_directory` and `run.arguments`. Pass arguments after `--`. Run the program in a terminal when it expects input. |
| A reload fails but the game still runs | Read the candidate's diagnostic. The host can keep the previous module active. Fix the source, compile again and activate only a successful candidate. |
| An iterator or pointer stops working after a mutation | Check whether collection growth, removal, arena reset/disposal or owner destruction ended the borrow. Acquire a fresh reference after the change. |

[Documentation index](README.md) · [Command-line reference](Command-Line.md) · [Build profiles](Projects.md)
