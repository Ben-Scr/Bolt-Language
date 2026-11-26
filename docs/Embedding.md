# Embedding Bolt and reloading scripts

A native application can compile source in memory, load it as a module and call its entry point. For a game editor, the important distinction is between **compiling a candidate** and **activating it**. Keep running the active module until the candidate compiles and loads successfully.

## Run a complete Funke example

From the Bolt source directory, with CMake and a C++20 compiler installed, build the sample against the desktop SDK. Here the source tree also contains the SDK's `bin`. In PowerShell and POSIX shells, `$PWD` expands to the current directory. If your SDK is elsewhere, use its absolute directory instead.

```sh
cmake -S examples/ExternalFunkeHost -B build/funke-host "-DBOLT_SDK_ROOT=$PWD"
cmake --build build/funke-host --config Release
ctest --test-dir build/funke-host -C Release --output-on-failure
```

`Funke.HotReload` runs a headless scene. An entity starts at X = 2. Its first tick moves it to 3. A valid edit doubles the movement rate and the next tick reaches 5. After a rejected edit, the last valid script moves it to 7. The host owns the transform throughout. Inspect the four `extern "C"` declarations in the script and their registrations in the adapter to see how a Bolt operation reaches the engine. [Movement script](../examples/FunkeBolt/Move.bolt) · [C++ adapter](../examples/FunkeBolt/BoltSceneScript.hpp) · [Buildable host](../examples/ExternalFunkeHost)

## Compile, load, run and release

Include `<bolt/CompilerHost.h>` and link the matching `BoltCompiler` library. SDK consumers using CMake can reuse the example's `bolt_import_compiler` and `bolt_link_compiler` functions. Keep the compiler DLL or shared library discoverable when the host starts. [C ABI declarations](../include/bolt/CompilerHost.h) · [CMake import](../examples/ExternalFunkeHost/BoltCompilerSDK.cmake)

The following C program compiles a script entirely in memory and prints its integer result. The status returned by the API reports whether the operation succeeded. The separate `result` is the value returned by Bolt's `Main`.

```c
#include <bolt/CompilerHost.h>
#include <stdio.h>

int main(void)
{
    const char source[] =
        "public static class Program { public: int Main() { return 42; } }";
    char error[2048] = {0};
    BoltCompiler *host = NULL;
    BoltCompiledModule *module = NULL;
    int32_t result = 0;

    int status = bolt_compiler_create(BOLT_COMPILER_HOST_ABI, BOLT_HOST_DEBUG,
                                      NULL, 0, &host, error, sizeof error);
    if (status != BOLT_HOST_OK) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    status = bolt_compiler_compile(host, "Main.bolt", source, sizeof source - 1,
                                   &module, error, sizeof error);
    if (status == BOLT_HOST_OK)
        status = bolt_compiler_load(host, module, error, sizeof error);
    if (module) bolt_module_release(module);
    if (status == BOLT_HOST_OK)
        status = bolt_compiler_run(host, &result, error, sizeof error);

    if (status == BOLT_HOST_OK) printf("%d\n", (int)result);
    else fprintf(stderr, "%s\n", error);
    bolt_compiler_destroy(host);
    return status == BOLT_HOST_OK ? 0 : 1;
}
```

In an MSVC developer terminal at the source root, save that program as `host.c` in a scratch directory. Build with `cl /nologo /Ibin/include C:\work\host.c /link /LIBPATH:bin BoltCompiler.lib`, substituting your source path. Put the SDK's `bin` on `PATH` before running the result. On a POSIX desktop use your SDK include directory, link `-lBoltCompiler` and configure the platform's runtime library search path.

## Module and session contracts

| Operation | Contract |
|---|---|
| `bolt_compiler_create` | Choose `BOLT_HOST_DEBUG` or `BOLT_HOST_RELEASE`. Supply the header's ABI constant, callback bindings and an output handle. `bolt_compiler_host_abi()` reports the loaded library's ABI. |
| `bolt_compiler_compile` / `bolt_compiler_compile_project` | Build an independent candidate from counted UTF-8 bytes, a file or a project. Only its creating compiler may load it. |
| `bolt_compiler_compile_sources` | Pass an array of named `BoltSource` buffers and an optional entry. Diagnostics preserve the source names and original positions. |
| `bolt_compiler_check_project` / `bolt_module_stats` | Check a project without loading code, or read parse reuse and frontend/native code generation durations from a compiled module. Durations are in nanoseconds. |
| `bolt_compiler_load` | Acquires its own reference to the candidate. Passing `NULL` unloads the current module. Release the caller's candidate reference after the load attempt. |
| `bolt_compiler_run` | Run the active module. A normal module invocation starts fresh. A call takes a snapshot of the active image, so an in-flight invocation can finish across a reload. |
| `bolt_compiler_run_persistent` | Retains module statics and allocations across calls. Each call receives a fresh step budget. Replacing the module starts a new session. Replace a failed persistent session before reuse. |
| `bolt_compiler_visit_fields` | Visit field declarations and attributes without executing source. Copy any strings needed after the visitor returns. |
| `bolt_compiler_version` | Read the compiler version from the loaded library. |
| `bolt_compiler_bind_pointer` / `bolt_compiler_bind_submit` | Register a pointer-returning callback or a native worker submission callback before compiling sources that declare it. |
| `bolt_module_visit_layout` | Read the size, alignment and field offsets of a compiled plain struct, including nested fields. Copy the descriptors during the visit. |
| `bolt_module_release` / `bolt_compiler_destroy` | Release owned references. Finish all uses of a handle before destroying it. |

Pass a UTF-8 diagnostic buffer and inspect it on failure. `BOLT_HOST_NO_MODULE` indicates that a run was requested without an active module. An editor can retain its last working module after a compilation error simply by leaving it loaded. Host-owned state, such as a transform, survives a reload by design. Decide explicitly which script data the host should restore in a replacement module.

### Callbacks

A `BoltHostBinding` pairs a name with a C callback accepting two `uint64_t` values and returning one `uint64_t`. The script declares `extern "C" ulong Name(ulong first, ulong second);`. Agree on units at the boundary. The movement example uses microseconds for elapsed time and thousandths of a world unit for distance. Keep callback implementations and their backing state alive until all using modules are released and in-flight calls have returned. Callbacks must return normally across the C boundary. [Host behavior and rollback tests](../tests/compiler_host.py)

A host can bind a native borrow with the Bolt signature `void*(ulong, ulong)`. The host must keep the pointed-to storage valid for the duration of the borrow. A submission binding takes `ulong, void*, void*` and returns a `ulong` handle. Before releasing a module, drain worker callbacks that can still enter its generated code. Funke uses these signatures for ECS component access and its existing job pool. Both native JIT profiles accept these bindings.

## Other embedding routes

- **C++ interpreter API.** Include `<bolt/Bolt.hpp>`, use `bolt::compile_source` or `bolt::load`, then create a `bolt::Runtime`. `run` invokes `Main`. `call` invokes a public static method or mutates a supplied instance. `create` produces a Bolt value. [Complete C++ example](../examples/embedding/main.cpp) · [API header](../include/bolt/API.hpp)
- **C interpreter API.** `bolt_interpreter_compile` prepares a program. `bolt_interpreter_run` accepts UTF-8 input bytes, arguments and a write callback. Static state persists between runs. The cached compiler form reuses syntax across edits. Release both program and compiler handles after use. Set instruction, call-depth and nested-runtime budgets through `BoltInterpreterLimits`. The I/O callback's bytes are valid during that callback. [Interpreter header](../include/bolt/InterpreterHost.h)
- **Foreign native libraries.** `[Header("ffi.h"), Native("symbol")]` connects a Bolt declaration to an external header and symbol. Supply include directories and link libraries. `extern "C++"` declarations can name C++ functions and native structs through the compatibility backend. [Runnable FFI example and build instructions](../examples/NativeInterop/README.md)
- **Precompiled mobile module.** Link the generated artifact and call `bolt_mobile_run(argc, argv)` on the engine thread. `argv[0]` is the first script argument. The result is the script exit code, or `1` for a runtime error. [Mobile entry header](../include/bolt/MobileAot.h) · [Platform builds](Platforms.md)

[Editor watcher](Editors.md) · [Projects and configuration](Projects.md) · [Documentation index](README.md)
