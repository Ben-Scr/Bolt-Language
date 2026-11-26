# Bolt unter Linux mit VS Code debuggen

Öffne den Ordner `LinuxDebug` in VS Code. Installiere die Bolt-VSIX ab 0.2.2 und die Microsoft-Erweiterung *C/C++*. Im Terminal von VS Code müssen `bolt version` und `gdb --version` funktionieren. In einer Remote-SSH-Sitzung gehören beide Werkzeuge und die Erweiterungen auf den Linux-Rechner, auf dem das Programm läuft.

Öffne `src/Main.bolt` und setze mit **F9** einen Haltepunkt auf `int nextPosition = position + velocity;`. Starte mit **F5** die Konfiguration *Bolt unter Linux (GDB)*. Der Build-Task erzeugt für x86-64 und ARM64 mit dem Fast-Backend ein ELF mit DWARF-Quellzeilen. GDB hält in `Movement.Advance`. Mit **F5** läuft das Programm weiter und gibt `42` aus.

Für ein eigenes Projekt kannst du `.vscode/tasks.json` und `.vscode/launch.json` übernehmen. Der Task schreibt sein Debug-Programm nach `bin/Debug/BoltDebug`. Dieser Pfad steht in beiden Dateien. Fast-DWARF zeichnet Funktionen und Quellzeilen auf. Für die lokalen Bolt-Variablen liefert diese Version keine DWARF-Typ- und Speicherortinformationen.

[Microsoft-Dokumentation zur GDB-Startkonfiguration](https://code.visualstudio.com/docs/cpp/launch-json-reference)
