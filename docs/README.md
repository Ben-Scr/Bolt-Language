# Bolt documentation

Bolt is a statically typed language for standalone native programs and scripts embedded in an application. A single project can supply a command-line executable, run in a compiler host while someone edits it, or supply source for a mobile or browser build. The host and the chosen backend determine which facilities are available.

**New to Bolt?** Start with [Your first Bolt project](Getting-Started.md). It takes you from an empty directory to a scoreboard that accepts command-line arguments. To run a script inside an engine, continue with [Embedding and hot reload](Embedding.md).

| If you want to… | Read |
|---|---|
| Install the tools, write a program, read a diagnostic and run it again | [Getting started](Getting-Started.md) |
| Look up types, expressions, control flow and function declarations | [Language reference](Language.md) |
| Model game state, use interfaces and generics, or manage memory and callbacks | [Types, ownership and abstraction](Types-and-Ownership.md) |
| Choose a collection, read a file, serialize data or start a task | [Standard library guide](Standard-Library.md) |
| Find a type, its namespace, methods and overloads | [Library API index](Library-API.md) |
| Organize sources, add another project or configure build profiles | [Projects and configuration](Projects.md) |
| Choose an execution mode or look up a compiler flag | [Command-line reference](Command-Line.md) |
| Run from an editor, navigate symbols or debug a program | [Editors and debugging](Editors.md) |
| Call Bolt from C or C++, reload a script or integrate Funke | [Embedding and hot reload](Embedding.md) |
| Build for a different system, package a script or embed it in a web page | [Platforms and distribution](Platforms.md) |
| Build the compiler, run its tests, measure a change or diagnose a failure | [Toolchain development and troubleshooting](Development.md) |

Commands use `bolt` for the compiler on your path. In the Windows source tree you can call `.\bin\bolt.exe` directly. Paths such as `examples/Iteration` are relative to the Bolt source directory. Where a workflow needs an SDK, linker, editor extension or host program, the corresponding chapter lists it.

[Project overview](../README.md) · [Runnable examples](../examples) · [License](../LICENSE)
