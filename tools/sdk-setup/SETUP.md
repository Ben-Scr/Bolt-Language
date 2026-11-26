# Bolt für den eigenen Benutzer einrichten

Entpacke das SDK vollständig. Die Skripte brauchen die Datei `SHA256SUMS` und den Ordner `bin` aus demselben SDK. Sie installieren die fertigen Programme. Für Windows brauchst du weder Administratorrechte noch Python. Unter Linux, FreeBSD und macOS braucht das Setup Python 3.8 oder neuer.

## Windows x64 und ARM64

Öffne im entpackten Ordner ein Terminal und führe `.\setup.cmd` aus. Das Setup verwendet standardmäßig `%LOCALAPPDATA%\Programs\Bolt`. Befindet sich dort bereits eine ältere, separat verwaltete Installation mit `bolt.cmd`, installiert das Setup nach `%LOCALAPPDATA%\Programs\Bolt-SDK`. Es setzt den neuen `bin`-Ordner an den Anfang deines Benutzer-PATH. Beende Windows Terminal und VS Code vollständig, öffne ein neues Terminal und prüfe:

```powershell
Get-Command bolt
bolt version --json
```

Die erste Zeile zeigt, welche Installation dein Terminal tatsächlich verwendet. Die JSON-Ausgabe nennt die vollständige EXE, Architektur und Runtime-ABI. Wenn bereits ein anderes Programm namens `bolt` aus dem systemweiten PATH Vorrang hat, kannst du Bolt mit `& "$env:LOCALAPPDATA\Programs\Bolt\bin\bolt.exe" version` direkt ausführen.

Zur Deinstallation starte `.\uninstall.cmd` aus dem entpackten SDK oder das Skript `uninstall.cmd` im installierten SDK-Ordner. Ein anderer Installationsort ist mit `.\setup.cmd -Prefix "D:\Entwicklung\Bolt SDK"` möglich. Zur Deinstallation aus dem ZIP gib denselben Parameter an. Das installierte Deinstallationsskript ermittelt den Ort automatisch.

## Linux, FreeBSD und macOS

Wähle das SDK passend zu Betriebssystem und Prozessor. `uname -sm` zeigt beides an. Öffne den entpackten Ordner und starte das Setup.

```sh
sh ./setup.sh
```

Das Setup installiert nach `~/.local/share/bolt` und registriert den Suchpfad in den Startdateien für Bash und Zsh. Öffne ein neues Terminal und führe `command -v bolt` und `bolt version` aus. In einer bereits geöffneten Bash oder Zsh kannst du den Pfad mit `export PATH="$HOME/.local/share/bolt/bin:$PATH"` übernehmen. In Fish kannst du `fish_add_path ~/.local/share/bolt/bin` verwenden.

`sh ~/.local/share/bolt/uninstall.sh` entfernt die Installation. Ein anderes Ziel ist mit `sh ./setup.sh --prefix "$HOME/Programme/Bolt SDK"` möglich. Das Skript im installierten SDK kennt sein Ziel. Beim Aufruf aus dem entpackten Archiv gib denselben Parameter an.

Auf einem Mac mit vorhandener F4-Quellinstallation unter `~/.local/share/bolt` verwendet das vorgebaute SDK stattdessen `~/.local/share/bolt-sdk`. Nach dem Öffnen eines neuen Terminals zeigt `command -v bolt` den gewählten Compiler. Zum Entfernen des vorgebauten SDK in diesem Fall dient `sh "$HOME/.local/share/bolt-sdk/uninstall.sh"`. Ein belegtes Fremdverzeichnis erfordert ein anderes `--prefix`.

Die vorgebauten macOS-SDKs enthalten den Linker, Systemdeklarationen und LLVM 19. Mit `bolt backends` prüfst du die erkannte Toolchain. Für den C++-Backend werden die Xcode Command Line Tools benötigt.

## Wechsel vom älteren Windows-Installer

Findet das Setup die frühere Bolt-Installation unter `%LOCALAPPDATA%\Programs\Bolt` anhand ihrer Installationsdatei, installiert es das neue SDK daneben unter `%LOCALAPPDATA%\Programs\Bolt-SDK`. Sein `bin`-Ordner steht danach am Anfang des Benutzer-PATH. Öffne ein neues Terminal und prüfe den tatsächlich gestarteten Compiler mit `bolt version --json`. Das Feld `executable` zeigt den vollständigen Pfad. Mit `Get-Command bolt -All` kannst du weitere Treffer sehen. Eine explizite Einstellung `bolt.compilerPath` in VS Code hat Vorrang vor PATH.
   
## Updates und VS Code

Beende laufende Bolt-Prozesse und VS Code. Entpacke die neue SDK-Version in einen **separaten Ordner** und starte deren Setup mit demselben Installationsziel. Eigene Dateien im Ziel bleiben erhalten. Wenn du eine mitgelieferte Datei bearbeitet hast, zeigt das Setup ihren Namen an. Verschiebe sie für das Update vorher aus dem Installationsordner. Die Deinstallation bewahrt bearbeitete Dateien, fremde Dateien und vorher vorhandene PATH-Einträge.

Wenn das Kommando `code` verfügbar ist, installiert oder aktualisiert das Setup die beiliegende `Bolt-VSCode.vsix`. Alternativ kannst du in VS Code **Erweiterungen → … → Aus VSIX installieren…** wählen. Ein Wechsel des globalen Farbschemas bleibt deine Entscheidung. Bei der Deinstallation entfernt das Skript eine selbst installierte Erweiterung, wenn deren Version noch übereinstimmt. Eine bereits vor dem Setup vorhandene oder danach separat aktualisierte Erweiterung bleibt erhalten.

Mit `-SkipVSCode` bzw. `--skip-vscode` überspringst du die Erweiterung. Mit `-NoPath` bzw. `--no-path` installierst du das SDK ohne automatische PATH-Registrierung. Wenn du nur die separaten Setupskripte entpackt hast, übergib den entpackten SDK-Ordner mit `-Source "C:\Downloads\Bolt-Windows-x64"` oder `--source "$HOME/Downloads/Bolt-Linux-x64"`.
