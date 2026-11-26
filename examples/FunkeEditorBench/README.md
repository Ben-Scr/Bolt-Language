# Funke-Editor-Simulator für Bolt und C#

Der C++-Prozess hält eine kleine Szene mit 128 Entities im Speicher. Nach einem Skriptwechsel führt er einen Bewegungstick aus. Beide Sprachen lesen dasselbe Frame-Delta und ändern dieselben Transform-Daten über drei native Funktionen. Ein falscher Entity-Index, ein mehrfaches Update oder eine abweichende Prüfsumme bricht die Messung ab.

Bolt verwendet die öffentliche Compiler-Host-API mit nativem Fast-Debug-JIT. Für C# startet die App CoreCLR über hostfxr und ruft die originalen Compile- und FreeResult-Einstiegspunkte von Funkes Roslyn-Compiler auf. Der Compiler läuft im Editorprozess. Die erste Projektbewertung führt MSBuild einschließlich Offline-Restore aus. Danach behält die Sitzung Syntaxbäume, Referenzen und Compilerzustand im Speicher. Ein gültiges C#-Modul wird aus der erzeugten DLL in einen neuen entladbaren AssemblyLoadContext geladen. Die Welt und der C++-Prozess bleiben bei beiden Sprachen erhalten. Die Teilimplementierung der Engine-API verwaltet Delta-Zeit und Positionen. Sie simuliert einen logischen Frame ohne Rendering, Editor-UI und Dateiwatcher.

## Fertiges Paket verwenden

Der Vergleichstreiber verwendet die zur laufenden Python-Architektur passende Variante in `bin/windows-x64`, `bin/windows-arm64`, `bin/linux-x64`, `bin/linux-arm64`, `bin/macos-x64` oder `bin/macos-arm64`. Auf einem ARM64-System verwende ein natives 64-Bit-Python und das ARM64-SDK für .NET. Führe die folgenden Befehle im entpackten Verzeichnis `Bolt-Editor-Simulator` aus. Für die Teststeuerung wird Python 3.9 oder neuer benötigt. Der C#-Vergleich braucht das .NET-10-SDK. Es stellt Runtime, MSBuild, Referenzassemblies und Roslyn bereit. Die Projektdateien kommen ohne NuGet-Pakete aus. Unter Windows eignet sich PowerShell im entpackten Verzeichnis.

```sh
python compare.py --json ergebnis.json
python compare.py --languages bolt --files 2 --runs 20 --json nur-bolt.json
python compare.py --languages bolt csharp csharp-nosymbols --runs 100 --warmup 8 --json alle-profile.json
```

Unter Linux kann `python3` verwendet werden. Mit `--dotnet`, `--bolt-library` und `--bin` lassen sich explizite Pfade wählen. Die Ergebnisdatei enthält jede Einzelmessung, die Reihenfolge der Sprachprofile, Compiler- und Programmprüfsummen sowie die erwarteten Szenenprüfungen. [Linux-Referenzmessung](Ergebnisse.md)

## Simulator aus den Quellen bauen

Dafür werden eine C++20-Toolchain, ein zum Rechner passendes Bolt-SDK mit `bin/include/bolt/CompilerHost.h`, Python und das .NET-10-SDK benötigt. Unter Windows die x64 Developer PowerShell von Visual Studio mit den C++-Build-Tools öffnen. Die folgenden Befehle starten im Verzeichnis dieser README.

```sh
python build.py --sdk /pfad/zum/Bolt-SDK --output ./bin
python compare.py --bin ./bin --bolt-library /pfad/zum/Bolt-SDK/bin/libBoltCompiler.so --json ergebnis.json
```

Unter Windows den tatsächlichen Pfad zu `BoltCompiler.dll` verwenden. Auf einem Rechner mit mehreren SDKs legt der Treiber das entdeckte .NET-10-SDK für das erzeugte Testprojekt fest. Die Quellen des verwalteten Compilers und der originale NativeEngineAPI-Header stammen aus Funke-Commit `67e570bf2940b975185d0f000aba01aafe6ffbd6`. Beide sind unter Funkes MIT-Lizenz enthalten. [Compilerherkunft](managed/FunkeCompiler/ORIGIN.md) · [Engine-ABI](Funke/NativeEngineAPI.hpp)

## Messgrenzen

- Ein neues Programm startet mit Geschwindigkeit 4.000. Bei jedem Edit ändert sich die Konstante in einer Skriptdatei. Mit `--edit body` wechseln zusätzlich eine Verzweigung sowie Addition und Subtraktion. `--edit declaration` ändert die Parameter einer aufgerufenen Methode, ihren Rumpf und die Aufrufstelle. Ein unabhängiges Python-Modell prüft für jede Variante die resultierende Szene. Der Test verwendet zwei bzw. 32 Benutzerdateien. Im größeren Projekt durchläuft die Bewegung eine Kette von 30 weiteren statischen Klassen, deren Ergebnisse in jedem Entity-Update eingehen.
- Die erste Messung erfasst Prozessstart, Backend-Initialisierung, erste Projektkompilierung und ersten Frame. Pro Szenario und Profil gibt es eine Kaltstartbeobachtung. Das Betriebssystem kann Dateien bereits im Cache haben.
- Für warme Messungen beginnt die Uhr nach dem Speichern der Quelle beim Eingang des Reload-Befehls im C++-Prozess. Er misst Kompilieren, Veröffentlichen und ersten Skriptaufruf einzeln. Der erste C#-Aufruf enthält die benötigte native JIT-Kompilierung. Die Bolt-Kompilierphase erzeugt bereits ein natives JIT-Modul. Für einen Vergleich der Benutzerlatenz zählt die Gesamtzeit bis zum Ende des ersten Frames.
- Alle Sprachprofile erhalten dieselben Edits. Innerhalb von Blöcken durchläuft der Treiber alle Sprachreihenfolgen in zufälliger Permutation. Die Rohdaten nennen den Seed. Das p95 ist der Wert an Rang `ceil(0,95 × n)` der sortierten Messwerte.
- C# verwendet Debug, geprüfte Ganzzahlarithmetik und standardmäßig portable Debugsymbole. `csharp-nosymbols` wiederholt dieselbe Arbeit ohne PDB. Der Bolt-Fast-JIT erzeugt keine Debugsymbole. Ein Syntaxfehler wird bei jedem Profil abgefangen. Danach prüft der Simulator einen weiteren Tick der alten Fassung und die erste Ausführung der reparierten Fassung.
- Die Prüfsumme wird nach dem Timer aus allen 128 Endpositionen gebildet. Der Python-Treiber berechnet sie unabhängig mit ganzzahliger Referenzrechnung und kontrolliert auch zurückgehaltene oder reparierte Module. Dateiwatcher, Render-Thread und der übrige Funke-Editor liegen außerhalb dieses Tests.

### C++-Host direkt bedienen

`BoltEditorSim bolt <BoltCompiler-Bibliothek> <Projektverzeichnis>` startet die native Sitzung mit den Hostfunktionen aus [Movement.bolt.in](fixtures/Movement.bolt.in). Der Prozess erwartet die Zeilen `reload`, `frame` und `quit`. `reload` kompiliert und veröffentlicht das Projekt und führt den ersten Tick aus. `frame` führt das aktive Modul nochmals aus. Die Ausgabe enthält Compile-, Publish-, Frame- und Gesamtzeit in Nanosekunden sowie die gewichtete Positionssumme. Für die vollständige C#-Startzeile erzeugt der Vergleichstreiber die Pfade zu hostfxr, Bridge, Projekt, Skriptdatei, Ausgabebibliothek und dotnet.

## Nur Bolt auf Linux ARM64 messen

Auf einem 64-Bit-Raspberry-Pi oder einem anderen Linux-ARM64-Rechner kannst du den nativen C++-Simulator ohne .NET-SDK bauen. Du brauchst Python 3, einen C++20-Compiler und das zur CPU passende Bolt-SDK. Die Messung prüft bei jedem Edit den Zustand von 128 Entities und nach einem Syntaxfehler den nächsten Tick mit der zuletzt gültigen Skriptfassung.

```sh
python3 build.py --bolt-only --sdk /pfad/Bolt-Linux-arm64 --output ./bin/linux-arm64
python3 compare.py --bin ./bin/linux-arm64 --languages bolt \
  --bolt-library /pfad/Bolt-Linux-arm64/bin/libBoltCompiler.so \
  --files 2 32 --warmup 5 --runs 40 --json vergleich-arm64.json
```

Für den gemeinsamen Bolt-/C#-Vergleich baue den Simulator mit installiertem .NET-10-SDK ohne `--bolt-only`. Ein echter Raspberry Pi und eine emulierte ARM64-Maschine benötigen getrennte Messreihen.


## Auf dem eigenen Editor-PC messen

Für denselben Methodenvergleich wie im 0.16-Messbericht im entpackten Simulator-Verzeichnis in PowerShell ausführen. Der Vergleichsprozess startet mit einem leeren Testprojekt und führt dieselben Änderungen in beiden Sprachen aus.

```powershell
python compare.py --edit body --files 2 32 128 --languages bolt csharp csharp-nosymbols --warmup 11 --runs 121 --json funke-methoden.json
```

Bei mehreren parallelen .NET-Installationen kann `--dotnet "C:\Program Files\dotnet\dotnet.exe"` den Host festlegen. Der Treiber wählt die neueste stabile 10er-SDK- und Hostversion aus diesem Installationsstamm. Reine Preview-Installationen werden in numerischer Versionsreihenfolge berücksichtigt. Die Ergebnisdatei speichert die benutzten Versionen und Prüfsummen.

Für die Zeit eines vollständigen Release-Builds steht ein zweiter Versuch zur Verfügung. Er startet den Bolt-Compiler und Roslyn pro Übersetzung neu. Jeder Lauf erhält ein neues Quellverzeichnis und einen leeren Ausgabeordner. Die Ausführung zur Überprüfung des Ergebnisses liegt außerhalb des Messfensters. Bolt erzeugt eine native Datei und C# eine optimierte IL-Assembly. MSBuild, Restore und C#-JIT-Laufzeit fließen in diesen Vergleich nicht ein.

```powershell
$compiler = (bolt version --json | ConvertFrom-Json).executable
python release_cli.py --bolt "$compiler" --files 2 32 128 --runs 12 --json funke-release-cli.json
```
