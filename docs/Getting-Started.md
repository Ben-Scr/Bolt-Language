# Your first Bolt project

You will build a scoreboard that reads player names, assigns points and prints the result. You need a Bolt compiler for your machine. The first project uses the standard library and the native build tools shipped with or detected by your SDK.

## Make the compiler available

**From this Windows source tree**, open PowerShell at the root and add its `bin` directory to this terminal's search path. You can now follow the rest of the tutorial from a separate working directory.

```powershell
$env:Path = (Resolve-Path .\bin).Path + ';' + $env:Path
Get-Command bolt
bolt backends
```

**From an SDK archive**, extract the whole archive. On Windows run `.\setup.cmd` in the extracted directory. On Linux, FreeBSD or macOS run `sh ./setup.sh`. Open a new terminal afterwards. To use an extracted desktop SDK in place, run the compiler in its `bin` directory. Keep the runtime libraries and linker files alongside it. The [SDK setup guide](../tools/sdk-setup/SETUP.md) covers custom installation directories, shell setup, uninstalling and the VS Code extension.

`bolt backends` prints the host target and the native code generators that this installation can load. `bolt version --json` reports the executable path and ABI identifiers. These identify the tools involved when a terminal and an editor behave differently.

## Create and run the project

Open a terminal in a directory where you keep your own projects. Choose a new directory name for the project. The following commands work in PowerShell and POSIX shells.

```sh
bolt init Scoreboard
cd Scoreboard
bolt check .
bolt run .
```

The generated program prints `Hello from Bolt!`. The project manifest is `bolt.proj`. The entry point is in `src/Main.bolt`. Keep this terminal in `Scoreboard` for the remaining steps.

## Give the program some input

Replace the contents of `src/Main.bolt` with the following program.

```bolt
import System.Collections.Generic

public static class Program
{
public:
    int Main(string[] args)
    {
        List<string> names;
        for (string name : args)
            names.Add(name);

        if (names.Count == 0) {
            Print("Pass at least one player name.");
            return 1;
        }

        for (int i = 0; i < names.Count; i++) {
            int score = (i + 1) * 10;
            Print($"{names[i]} scored {score}");
        }
        return 0;
    }
}
```

`Main` receives the arguments following `--`. It returns a process exit code. `List<string>` holds strings, the range loop visits each argument and `$"…"` inserts expression values into the output.

```sh
bolt run . -- Ada "Sam Lee"
```

```text
Ada scored 10
Sam Lee scored 20
```

A run without names prints the usage message and exits with code `1`. The compiler writes build timing to the diagnostic stream. Add `--no-timings` before `--` to suppress it.

## Read a compiler diagnostic

Temporarily change `names.Count` in the first `if` to `namse.Count`. Run `bolt check .`. The diagnostic identifies the source file, position and unknown name. Restore `names` and check again. For machine-readable output, use `bolt check . --diagnostics json`. This workflow also catches mistakes in a different source file when both files belong to the project.

## Build once and run again

```sh
bolt compile . --release --require-native
bolt run . --release --no-compile -- Ada
bolt project info . --format json --profile Release
```

The second command prints `Ada scored 10` using the saved executable. The JSON field `output_directory` tells you where the compiler put it. A direct release build needs the LLVM backend and a working linker. For everyday edits use `bolt run .` to rebuild changed source.

To keep working on the example, add a type in `src/Player.bolt` and use it in `Main`. Bolt discovers both files under the project's source root. [Language reference](Language.md) · [Projects and configuration](Projects.md) · [Hosting the first script](Embedding.md)
