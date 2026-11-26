# Platforms and distribution

Choose the host that will run the compiler and the target that will run the resulting program. A desktop SDK supplies a compiler for its host. A cross-build also needs the selected target's object format, ABI, runtime and system libraries. `bolt backends` describes the local installation.

| Destination | Build route | Deliverable |
|---|---|---|
| Windows desktop | Native AOT or C++ compatibility toolchain with a matching Windows SDK/linker | Windows executable or project library, plus runtime dependencies |
| Linux desktop | Native AOT or C++ compatibility toolchain with the intended architecture and libc | ELF program or library |
| macOS desktop | Mach-O build with the target architecture, SDK and deployment target | macOS executable or library |
| FreeBSD desktop | FreeBSD toolchain and matching SDK/runtime | FreeBSD executable or library |
| Android | Desktop Bolt emitter and Android NDK toolchain | Static script archive for one Android ABI |
| iOS and simulator | Desktop emitter and Apple toolchain or supported cross toolchain | Script XCFramework archive |
| Browser | Desktop emitter linked by Emscripten, or the browser interpreter SDK | ES module and WebAssembly companion, or source executed through the interpreter |

## Desktop builds

For an executable project, start with `bolt compile . --release` and read the artifact path in its output. Use `--require-native` when direct code generation is a requirement. A linked program may depend on `BoltRuntime` and other shared libraries. Package the dependencies for that specific build. Test the distributed folder on the intended target, with the developer SDK removed from the executable search path. Keep compiler, runtime and headers from a matching build. For native dependencies see the `[native]` manifest settings. [Projects](Projects.md)

For a Linux cross target, use a GNU/Linux-hosted Bolt compiler built against glibc and an LLVM library with support for the target. From that environment, this command writes an ARM64 object from a standalone source file.

```sh
bolt compile examples/iOSAot/Scene.bolt --release --backend llvm --target aarch64-linux-gnu --object-only -o Scene.o
```

For a linked cross executable, supply the required target runtime with `--runtime-dir`, the system library tree with `--sysroot` and a suitable `--linker`. Supported Linux target spellings include `x86_64-linux-gnu`, `aarch64-linux-gnu` and `riscv64-linux-gnu`. A GNU/Linux target and a musl-based desktop package use different runtime conventions. Run the result on the matching target or an appropriate emulator. For macOS keep the application and its runtime compatible with the selected deployment target. [Linux cross-build examples](../tests/linux_cross.py) · [macOS SDK builder](../tools/build-macos-sdk.py)

## Browser export

For a precompiled browser program, put `bolt` and Emscripten's `em++` on the path and use a Bolt SDK with the current headers under `include/bolt`. From the Bolt source directory, substitute your SDK path in the following command.

```sh
python tools/bolt-web-build.py examples/Projects12/Hello --sdk /path/to/Bolt-Web -o out/hello.mjs
```

Deploy `hello.mjs` and `hello.wasm` together. Serve them over HTTP from the same directory. The ES module exports an initializer. After initialization, `callMain` accepts an array of script arguments. With `--debug` the exporter also produces a source map. [Web build tool](../tools/bolt-web-build.py)

Threaded exports create their worker pool during module initialization. Serve them with `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` so the browser enables shared memory. The exporter preloads eight workers by default. Set `--workers N` for workloads that need more simultaneous blocking threads. A full pool reports a thread-start failure. For responsive UI during blocking Bolt calls, invoke `callMain` from an application Web Worker. SDKs that expose the interpreter to threaded Web programs can provide a `lib/libBoltInterpreter-mt.a` built with `-pthread`.

```javascript
import initialize from "./hello.mjs";

const app = await initialize();
app.callMain([]);
```

For an interactive source editor, the **browser interpreter** exposes a different API. Load its module, use `evaluate` for a one-shot check/run or `prepare(source, stepLimit)` for repeated execution. Check `ok` before reading `program`. Call `program.run(inputText, args)`, read `stdout` and `exitCode`, then call `program.delete()`. Prepared programs retain static state. `new bolt.Compiler()` can reuse syntax across edits and must also be deleted. [Browser interpreter usage](../tests/browser_runtime.mjs)

## Android and iOS

For Android, extract a Bolt Android SDK and install the NDK. Each invocation produces one architecture's archive. The helper accepts `arm64-v8a`, `armeabi-v7a`, `x86_64` and `x86` as ABI names.

```sh
python tools/bolt-android-aot.py examples/iOSAot/Scene.bolt --abi arm64-v8a --sdk /path/to/Bolt-Android --ndk /path/to/android-ndk -o out/libscene.a
```

Link that archive and the matching SDK runtime into the native application. For iOS the exporter builds device and simulator slices and packages them as an XCFramework. Use an Apple development environment with the required SDKs, or configure the cross toolchain accepted by the helper.

```sh
python tools/build-ios-aot.py examples/iOSAot/Scene.bolt --output Scene-iOS.zip
```

Add the script XCFramework and the required matching runtime to the application. Call the symbol declared in `bolt/MobileAot.h`. Pass `0, NULL` for no script arguments. If your application links multiple scripts, assign each one a distinct C entry point with `--symbol`. The sample scene returns `17` without arguments and `19` with two. [Sample C host](../examples/iOSAot/host.c) · [Android helper](../tools/bolt-android-aot.py) · [iOS helper](../tools/build-ios-aot.py)

## Check platform-specific source before exporting

```sh
bolt check examples/iOSAot/Scene.bolt --target ios-arm64
bolt check examples/iOSAot/Scene.bolt --target web
```

The same `--target` on `bolt emit` selects the target conditionals during C++ emission. Hosted target spellings are `web`, `android-arm64`, `android-x64`, `android-armv7`, `android-x86`, `ios-arm64`, `ios-sim-arm64` and `ios-sim-x64`. A frontend check verifies active source and types. To verify linking, OS services or deployment, run the packaged artifact on the target.

[Command-line options](Command-Line.md) · [Host interfaces](Embedding.md) · [Documentation index](README.md)
