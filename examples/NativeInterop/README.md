# Native interop: real C code and C++ types

This example calls a compiled C archive (`ffi.c`) and a header-defined C++ function
and class (`ffi.h`). No dynamic code generation, runtime name lookup or unsafe
reinterpretation is used to turn the C++ class into a Bolt class.

From this directory, with the rebuilt Bolt 0.7 compiler installed:

```sh
cmake -S . -B build
cmake --build build --config Release
bolt compile Program.bolt --backend cpp --header-dir . --link-lib build/lib/libexample.a
```

For multi-configuration Windows builds the library is normally
`build/lib/Release/example.lib` instead. Use that actual path as `--link-lib`.
Run the resulting executable from `bin/Debug` (the compiler prints its path).

`[Header("ffi.h")]` selects a C/C++ header. `[Native("native_add")]` identifies an
actual external symbol, not its Bolt alias. `extern "C"` uses C linkage. `extern
"C++"` explicitly relies on a compatible C++ compiler, standard library and ABI.
The native struct binding declares the members Bolt code is allowed to access;
its true layout and construction are resolved by the C++ compiler against the header.

A `const string&` input pins UTF-8 bytes for the call. Returned `const char*` strings
are copied while valid, and must be non-null and NUL-terminated. Reference parameters
in a C declaration become pointer parameters at the ABI boundary. Arrays pass a
contiguous data pointer; pass a separate length as shown. The foreign function must
not overrun or retain borrowed buffers beyond the call.

This is **not** automatic C++ header import or interoperability between arbitrary
C++ ABIs. Use a fixed-signature C wrapper for C varargs, incompatible C++ toolchains,
complex ownership conventions, callbacks or types not representable by the binding.
No exception may cross an unrelated C ABI boundary. Native code can still corrupt
memory; Bolt cannot make an unsafe foreign library safe.

The example flushes C stdout after `puts` before Bolt writes through the operating-system console. This preserves the order of the two APIs when output is redirected to a pipe.
