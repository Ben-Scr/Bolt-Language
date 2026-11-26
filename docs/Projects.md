# Projects and configuration

A Bolt project describes which sources form a program or library. Put the shared project definition in `bolt.proj`. Use the optional `bolt.conf` beside it for language, build and run settings. Both files use Bolt's TOML schema. From a new application's parent directory, `bolt init Scoreboard` creates this layout.

```text
Scoreboard/
  bolt.proj
  src/
    Main.bolt
```

## The manifest

This complete manifest builds an executable called `Scoreboard` from all eligible `.bolt` files under `src`.

```toml
format = 1

[project]
name = "Scoreboard"
kind = "executable"

[sources]
roots = ["src"]
```

| Key | Meaning |
|---|---|
| `format` | Manifest schema integer. Use `1`. |
| `project.name` | Required. A portable filename component used for the output artifact. |
| `project.kind` | `executable` (default), `static-library` or `shared-library`. A library has no executable entry point. |
| `project.entry` | Optional class that supplies an executable's public static `Main`. A qualified name such as `Game.Startup.Main` disambiguates the class. The default class is `Program`. |
| `project.version` | Optional descriptive string for the application or library being built. |
| `sources.roots` | Nonempty list of existing source directories, relative to the project by default. Defaults to `["src"]`. |
| `dependencies` | Named local library projects. Each entry has a `path` to a directory containing `bolt.proj` or to the manifest itself. |
| `native.include_dirs` | Directories containing headers for foreign declarations. |
| `native.library_dirs`, `native.libraries` | Search directories and library names/files for linking. |
| `native.runtime_dirs` | Directories containing native runtime dependencies. |

Keep paths spelled with their actual on-disk case. Source discovery descends through the configured roots and skips generated directories and nested project boundaries. Point `bolt run` at the **project directory** when you want manifest settings and local dependencies. Giving the compiler a `.bolt` file explicitly selects the file workflow.

## Add a local library

Suppose `App` and `Math` are sibling directories. Set `kind = "static-library"` in the `[project]` section of `Math/bolt.proj`. Add the following table to `App/bolt.proj`.

```toml
[dependencies]
GameMath = { path = "../Math" }
```

In `Math/src`, declare public types under `namespace GameMath;`. In the application, `import GameMath` makes those names available. The dependency declaration locates the project. The namespace in the sources controls name lookup.

From the Bolt source directory, `bolt project info examples/Projects12/App --format json` shows the resolved library and sources. `bolt run examples/Projects12/App --require-native` builds the static library, links the application and prints `42`. Fast and LLVM can both build `static-library` and `shared-library` projects directly. The selected backend and `--require-native` apply to the whole dependency graph. For separately linked projects, Bolt places the shared runtime and shared dependencies beside the executable. [Application](../examples/Projects12/App) · [Library](../examples/Projects12/Math)

To run the same example without BoltRuntime, use `bolt run examples/Projects12/App --no-runtime`. Dependencies can be static or shared, including a shared library that calls another shared library. LLVM checks reachable calls and static initializers across the source graph using each project's source configuration. Shared projects produce native dynamic libraries. Deploy the executable and its adjacent libraries together. All images use the same Bolt static fields and type descriptors.

To build a shared library directly, run `bolt compile <library-project> --no-runtime`. Without an application entry point, the compiler checks the library's methods and its dependency initializers. Application builds select the methods they use. An independently built library can replace a deployed library when its exports and bindings to application code and state match. Rebuild the consuming project when those bindings change.

Constructs such as foreign C++ bindings still require `--backend cpp`. Bolt rejects dependency cycles, executable dependencies, conflicting artifact names and source files owned by multiple projects.

## Configure a profile and run defaults

Place this optional file at `Scoreboard/bolt.conf`.

```toml
format = 1

[language]
prelude = true
reflection = "opt-in"
defines = ["PLAYTEST"]

[build]
default_profile = "Debug"
target = "host"
incremental = true
parallel = true

[profiles.Debug]
optimization = "none"
debug_info = true

[profiles.Release]
optimization = "aggressive"
debug_info = false

[run]
working_directory = "."
arguments = ["Ada", "Sam Lee"]
```

`prelude` enables automatic imports including the console helpers. Local dependencies must use the same prelude setting as the main project. `defines` adds boolean conditional-compilation symbols. Apply `[Reflect]` on the individual types whose metadata your application needs.

Profiles are named `Debug` and `Release`. The profile selects source conditionals and supplies optimization and debug information settings. `incremental = false` forces a rebuild. `parallel` controls project build locking across profiles. `build.target` accepts `"host"`. Targeted exports and cross-compilation use explicit CLI options and matching toolchains.

Run arguments in `bolt.conf` apply when the command line supplies none. Arguments after `--` replace the configured list. A final `--` alone requests an empty list. Use `--working-directory PATH` with `run` to override the working directory for one invocation.

## Locate inputs and outputs

```sh
bolt project info . --format json --profile Debug
bolt project info . --format json --profile Release
```

Use `sources` for the project's own sources, `all_sources` for the resolved graph, `dependencies` for the local libraries and `run` for the launch context. In a manifest project, the typical outputs are `bin/<host-target>/<profile>/` and `obj/<host-target>/<profile>/`. `output_directory` and `intermediate_directory` give the effective paths. `BOLT_BUILD_ROOT` relocates generated artifacts outside the source tree with a distinct root for each source project. Files built with an explicit `-o` may live elsewhere.

`bolt new Name` creates a flat project with `Program.bolt` and a `bolt.project` marker. Examples also use loose files and folders. Use `bolt init` for the TOML layout described here. [Project schema and resolver](../src/Project.cpp) · [Command-line reference](Command-Line.md) · [Documentation index](README.md)
