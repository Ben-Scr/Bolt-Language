# Editors and debugging

The compiler supplies the language server. Select the same executable in the editor that you use in the terminal. That keeps project discovery, type checking and conditional compilation consistent across both.

## VS Code

1. Install the Bolt VSIX. The Windows source tree includes a package under `editors/vscode`. In VS Code open **Extensions → … → Install from VSIX…**. The desktop SDK setup can also install its bundled extension.
2. Open your application folder so `bolt.proj` and `src` share a workspace. Open `src/Main.bolt`. Use **Bolt: Select Compiler…** to select the SDK's `bolt` or `bolt.exe`.
3. Use **Bolt: Select Build and Analysis Profile** to pick the project default, Debug or Release. The selected profile also controls which `#if` blocks participate in diagnostics.
4. Run with the triangle in the editor or `Ctrl+F5`. For a command-line program that reads from the console, enter input in the run terminal. Use **Bolt: Show Toolchain Info** to see which compiler the extension launched.

| Task | Editor action |
|---|---|
| See the type of a variable or the signature of a method | Hover the identifier |
| Open a declaration or find its callers' references | `F12` for definition, `Shift+F12` for references |
| Rename a resolved symbol | `F2` |
| Add a missing import offered by the compiler | Use Quick Fix on the diagnostic (`Ctrl+.`) |
| Find syntax/type errors in unsaved edits | Check the Problems panel |
| Open an embedded library declaration | Go to Definition on the library member |
| Format code or navigate its structure | Format Document, Outline and document symbols |

Set `bolt.compilerPath` to an absolute path to pin the executable. `bolt.run.arguments` takes an array of strings. `null` inherits the project arguments, and an empty array requests zero arguments. `bolt.run.workingDirectory` sets the launch directory. `bolt.run.environment` maps names to strings. Example workspace settings for the scoreboard tutorial follow.

```json
{
    "bolt.profile": "Debug",
    "bolt.run.arguments": ["Ada", "Sam Lee"],
    "bolt.run.environment": { "SCOREBOARD_THEME": "dark" }
}
```

For a remote workspace, install the SDK and workspace extension where the sources and processes run. If diagnostics stop updating, inspect Bolt's output channel and use **Bolt: Restart Language Server**. [VS Code extension settings](../editors/vscode/package.json) · [Extension guide](../editors/vscode/README.md)

## Visual Studio 2022 and 2026

Install the [Bolt Language Tools VSIX](../build/visualstudio/Release/Bolt.VisualStudio.vsix) and the **Desktop development with C++** workload. From **Create a new project** or **File → New → Project**, search for **Bolt** and select **Bolt Console Application**. Choose a name and location, then select **Create**. The template includes `bolt.proj`, `src/Main.bolt`, and Debug and Release configurations. [Visual Studio extension guide](../editors/visualstudio/README.md)

## IntelliJ IDEA

Install the [Bolt plugin for IDEA 2026.2.3](../editors/packages/bolt-intellij-0.10.4.zip) using **Settings → Plugins → ⚙ → Install Plugin from Disk** and restart IDEA. On the welcome screen choose **New Project**, or use **File → New → Project**. Choose **Bolt**, enter the name and location, then click **Create**. Open `src/Main.bolt` in the Project view and run its `Main` entry point. Set the compiler under **Settings → Tools → Bolt** if needed.

For another supported IDEA build, run `python tools/build-intellij.py --ide <IDE installation>` to build the ZIP against that installation. [IntelliJ plugin builder](../tools/build-intellij.py)

## Source-level debugging

On Linux, the repository includes a working GDB launch configuration for VS Code. Install the Microsoft C/C++ debug adapter and GDB, open `examples/LinuxDebug` and set a breakpoint on the position calculation in `Movement.Advance`. Press `F5` with the Bolt/GDB launch configuration. Continuing the program prints `42`. The launch configuration and its build task must agree on the executable path. Fast debug information on this route records functions and source lines. Inspecting typed locals requires suitable debug information and debugger support. [Example workspace](../examples/LinuxDebug) · [Linux debugger instructions](../examples/LinuxDebug/README.md)

## Watch a project without an IDE

The watcher uses Python 3.11 or newer (`tomllib`) and the native compiler-host library. With both installed, run these commands from the Bolt source directory.

```sh
python bin/bolt-watch.py examples/Projects12/Hello --once
python bin/bolt-watch.py examples/Projects12/Hello
```

The first command compiles and runs once. The second watches sources, project files and local project dependencies, then recompiles after edits. Stop it with `Ctrl+C`. Use `--library PATH` to select the compiler library explicitly, `--watch-root DIR` for extra sources and `--interval SECONDS` to change the polling interval.

For editor automation start with `--commands`. Send one JSON object per input line, for example `{"op":"reload"}`, then `{"op":"run"}`. Other operations are `check` and `quit`. Read one response per line on standard output, including `ok` and `active_generation`. Script output goes to standard error. A failed reload leaves the prior generation available. [Watcher implementation](../tools/bolt-watch.py)

## Other language-server clients

Start `bolt lsp` as a subprocess with binary stdin/stdout. Use standard LSP framing, initialize the server, then send document open/change/close notifications. Use a `file:` URI for on-disk documents and read the capabilities returned at initialization. LSP positions count UTF-16 units from zero, including after a character represented by a surrogate pair. The protocol tests include unsaved edits, definition lookup, references and diagnostic clearing. [Protocol examples](../tests/lsp_protocol.py)

[Getting started](Getting-Started.md) · [Troubleshooting](Development.md) · [Documentation index](README.md)
