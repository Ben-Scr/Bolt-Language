# Command-line reference

Run `bolt help` to list commands and `bolt help run` or `bolt help compile` for the options exposed by your compiler. Examples below assume a terminal in a Bolt project.

## Commands

| Command | Result |
|---|---|
| `bolt init Name` | Create a new TOML project with `src/Main.bolt` in a new directory. |
| `bolt new Name` | Create a flat project with `Program.bolt` and a `bolt.project` marker. |
| `bolt project info . --format json` | Resolve project sources, dependencies, run settings and output directories. Accepts `--profile` and `--define`. |
| `bolt check .` | Parse and type-check. Use `--no-entry` for library code, `--diagnostics json` for tools. |
| `bolt compile .` or `bolt build .` | Build the selected executable or library artifact. |
| `bolt run .` | Build and launch an executable. Arguments following `--` go to the program. |
| `bolt exec 'Print(2 + 3);'` | Execute statements in the interpreter. `--jit` selects native in-memory compilation. |
| `bolt emit . -o Program.cpp` | Write the generated C++ source. A separate C++ toolchain can compile it against the Bolt headers and runtime. |
| `bolt emit-ir . -o Program.ll` | Lower a supported program to a textual LLVM representation for inspection. |
| `bolt backends` | Report the host, Fast and LLVM availability and the runtime surface. |
| `bolt version --json` | Print compiler identity, binary path, target and ABI identifiers. |
| `bolt lsp` | Start the Language Server Protocol over standard input/output. Editors start this process. |
| `bolt clean .` | Remove recognized project build artifacts and caches. It verifies ownership of generated paths. |
| `bolt help COMMAND` | Print usage for one command. |

## Choose how to run

| Command | When to use it | What survives the invocation |
|---|---|---|
| `bolt run . --debug` | Build an executable for development. Profile settings and target influence backend selection. | Executable and build records |
| `bolt run . --release --require-native` | Build an optimized executable using direct native code generation. | Executable and build records |
| `bolt run . --release --no-compile` | Launch the last successful build in the selected profile. Even edited or invalid source can have a saved executable. | Existing artifact |
| `bolt run . --jit` | Compile supported source into machine code and run it in the current process. | No executable file |
| `bolt run . --runtime` | Evaluate supported source with the interpreter. | No executable file |

For `--no-compile`, build the chosen profile at least once. Combine it with `--require-native` if the saved executable must come from a direct backend. `--runtime` takes application arguments and an optional step budget. It rejects explicit build profiles. `--jit` supports native backend selection. `--steps N` sets an execution budget for interpreter/JIT runs, and `0` disables that budget.

`bolt compile Program.bolt --release --no-runtime` checks the application's calls and static initializers before linking. The executable includes the Bolt implementations it uses, including UTF-8 strings and Unicode case mapping, typed array storage and copying, and exception handling with stack traces. `Main(string[] args)` receives the OS command-line arguments. Run `bolt run tests/native/path-components.bolt --no-runtime` from the source tree to exercise the path API. A call that still needs a host service, such as `Runtime.ExecuteProgram`, produces a build error.

`File.ReadAllBytes` and `File.WriteAllBytes` use native OS handles. `WriteAllText` and `WriteAllLines` create or truncate the file and write UTF-8 without adding a BOM. `AppendAllText` and `AppendAllLines` preserve existing bytes and create a missing file. The line-writing methods add LF after each supplied line on every platform. `ReadAllText` removes an optional leading UTF-8 BOM. `ReadAllLines` recognizes LF, CRLF and CR, preserves empty lines, and returns an empty array for an empty file.

Standalone executables support interface values and references, including interface elements and keys in generic collections. The compiler embeds the concrete type data used for boxing and dispatch. `Func` and `Action` support method groups, captured variables and invocation through delegate copies. Events use the same delegates for subscription, removal and invocation.

Standalone builds on Windows, macOS, FreeBSD and supported 64-bit Linux targets implement file and memory streams, text readers and writers, and little-endian binary readers and writers in Bolt. Stream copies share the current position and close state. On Windows and supported 64-bit Linux targets, standalone builds also implement the `File` and `Directory` APIs. File tasks run on OS threads and can be awaited. The program joins outstanding file operations at exit. Copies and moves reject an existing destination unless overwrite is enabled. Recursive directory traversal skips directory links.

## Build and launch options

| Option | Use |
|---|---|
| `--debug`, `--release`, `--profile Debug` or `--profile Release` | Select the profile. For manifest projects the configured default is used if omitted. |
| `--backend auto\|fast\|llvm\|cpp` | Select the code generator. Fast emphasizes build latency. LLVM supplies optimization and target support. C++ emits C++20 and invokes an external compiler. |
| `--require-native` | Require a direct native backend. Useful for pinning the build path and discovering an unsupported construct. |
| `--no-runtime` | Build a native LLVM executable or shared Bolt library without BoltRuntime. The compiler checks reachable calls and static initializers across local static and shared project dependencies. Direct operating-system imports are allowed. For an executable, Bolt deploys the required shared libraries beside it. With `run --no-compile`, Bolt verifies the saved executable and its deployed libraries. |
| `-o PATH` | Select the output. Required by `emit` and `emit-ir`. |
| `--define NAME` | Add a boolean source conditional. Repeat for additional symbols. |
| `--debug-info`, `--no-debug-info` | Override profile debug information. The requested information can affect which backend is eligible. |
| `--rebuild` | Ignore the unchanged-build cache for this invocation. |
| `--object-only` | Emit the direct backend's object file and stop before linking. |
| `--target TARGET` | Select a supported target for cross-compilation or hosted export. [Target workflows](Platforms.md) |
| `--llvm PATH`, `--linker PATH` | Select a shared LLVM library or native linker. Environment equivalents are `BOLT_LLVM_LIBRARY` and `BOLT_LINKER`. |
| `--runtime-dir DIR`, `--sysroot DIR` | Locate runtime/startup files and target system libraries. They must match the target ABI. |
| `--cxx COMPILER`, `--include DIR`, `--library FILE` | Select the compatibility compiler, Bolt header directory and BoltCore archive. |
| `--header-dir DIR`, `--link-lib FILE` | Add a foreign header directory or library. May be repeated. |
| `--lto`, `--no-pch` | Request link-time optimization for a C++ release build, or disable the GCC/Clang header cache. |
| `--verbose` | Show tool invocations for diagnosing compilation or linking. |
| `--timings`, `--no-timings` | Enable or suppress build/preparation timing on standard error. |
| `--diagnostics text\|msbuild` | Select human-readable or MSBuild-style diagnostics. `check` additionally accepts `json`. |
| `--working-directory DIR` | Set the program's working directory for `run`. |
| `-- arg1 "two words"` | End compiler option parsing and pass each following argument to `Main`. |

In automatic AOT mode Bolt can select the C++ compatibility path for a construct the direct backend cannot lower. Foreign C++ bindings require an appropriate compiler and libraries. `bolt backends` describes what the installation can load. To test a particular **program and specialization**, compile that program with the intended profile and `--require-native`. A successful `check` covers the frontend only.

## Read diagnostics in a tool

`bolt check . --diagnostics json` writes one JSON object to standard output. Inspect `diagnostics` for entries and check the process exit code. A successful check produces an empty diagnostics array. The payload identifies its coordinate encoding. Compiler positions use one-based lines and UTF-8 byte columns. The language server translates them into zero-based UTF-16 editor positions. Build/run text diagnostics normally go to standard error. A successfully launched program returns its own exit code through `bolt run`.

[Projects and configuration](Projects.md) · [Troubleshooting](Development.md) · [Documentation index](README.md)
