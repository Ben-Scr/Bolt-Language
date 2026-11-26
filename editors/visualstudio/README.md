# Bolt für Visual Studio

Bolt Language Tools unterstützt Visual Studio 2022 ab 17.14 und Visual Studio 2026 unter Windows x64. Eine VSIX enthält den Editorclient, die Syntaxgrammatik, die Bolt-Farbschemata und die Projekt- und Klassenvorlagen.

## Installation

1. [Bolt.VisualStudio.vsix](../../build/visualstudio/Release/Bolt.VisualStudio.vsix) doppelklicken und im VSIX Installer die gewünschten Visual-Studio-Instanzen auswählen. Eine frühere Version von Bolt Language Tools wird über dieselbe Erweiterungs-ID aktualisiert.
2. Visual Studio neu starten. Auf der Startseite **Neues Projekt erstellen** wählen, nach **Bolt** suchen und **Bolt Console Application** auswählen. Im geöffneten Visual Studio führt **Datei → Neu → Projekt** zum selben Dialog. Die Vorlage benötigt die C++-Projekttools aus der Workload **Desktopentwicklung mit C++**.
3. Unter **Extras → Bolt → Select Compiler…** bei Bedarf den Compiler wählen. In diesem Quellbaum liegt er unter `bin\bolt.exe`. Die automatische Suche berücksichtigt `BOLT_COMPILER`, den Suchpfad und die üblichen Bolt-SDK-Installationsorte. **Show Toolchain Info** zeigt Pfad, Version, Profil und Farbauswahl im Ausgabefenster.

## Im Editor

| Aktion | Bedienung |
| --- | --- |
| Bearbeiten | Die .bolt-Grammatik, Snippets für `main`, `class` und `for`, Kommentarbefehle, Klammerpaare, Einrückung und Blockfaltung stehen im Editor bereit. **Strg+K, Strg+D** formatiert das Dokument mit demselben Allman-Formatierer wie VS Code. |
| IntelliSense | Der Compiler liefert Diagnosen, Quick Fixes, Member-Vervollständigung, Signaturhilfe, Hover, Dokumentsymbole, Definitionen, Referenzen und Umbenennungen. **F12** öffnet auch schreibgeschützte Quellen der eingebetteten Standardbibliothek. **Strg+.** zeigt verfügbare Importkorrekturen. |
| Generische Typen | Bei aktivierter Klammervervollständigung ergänzt der Editor ein `>`, wenn der Compiler die generische Deklaration bestätigt. Ein Vergleich behält sein einzelnes `<`. |
| Farben | Die semantische und lexikalische Farbgebung verwendet die hellen und dunklen Bolt-Paletten aus den bestehenden Erweiterungen. Unter **Extras → Optionen → Bolt → General** lassen sich Farbschema und semantisches Highlighting einstellen. |
| Bauen und prüfen | **Build** und **Check** stehen im Bolt-Menü und im Editor-Kontextmenü. Compilerdiagnosen erscheinen in der Ausgabe und der Fehlerliste. **Cancel Build or Check** bricht den laufenden Vorgang ab. |
| Ausführen | **Strg+F5** startet die aktive Bolt-Datei oder das umgebende Projekt mit Ein- und Ausgabe in einer Konsole. An einem gespeicherten, vom Compiler bestätigten Projekteinstiegspunkt erscheint **▶**. |
| Debuggen | **F9** setzt einen Haltepunkt. **F5** baut mit LLVM und PDB-Symbolen und startet den nativen Visual-Studio-Debugger. **F10** und **F11** steuern Einzelschritte. Aufrufliste, Threads, Überwachung und Speicherfenster gehören zur nativen Debugsitzung. Welche lokalen Variablen und Felder sichtbar sind, hängt von den Debugdaten des verwendeten Bolt-SDK ab. |
| Projekte und Klassen | **New Project…** führt `bolt init` im gewählten Ordner aus. **New Class…** legt eine neue Quelldatei an. Mit installierten C++-Projekttools stehen zusätzlich **Bolt Console Application** unter **Neues Projekt** und **Bolt Class** unter **Neues Element** zur Wahl. |
| Profile und Startparameter | **Select Build and Analysis Profile…** wählt Projektstandard, Debug oder Release für die aktuelle Projektmappe bzw. den geöffneten Ordner. Für Einzeldateien gilt die globale Auswahl. Argumente, Umgebungsvariablen und Arbeitsverzeichnis stehen unter **Extras → Optionen → Bolt → General**. |
| Konfigurierte Tasks | **Run Configured Task…** liest Bolt-Tasks aus `.vscode/tasks.json` im geöffneten Ordner. Unterstützt werden `operation`, `target`, `profile`, `args`, `cwd` und `env`. |
| Analyse neu starten | **Restart Language Server** startet die Analyse mit dem ausgewählten SDK neu. Formatierung, lexikalische Farben, Faltung und Snippets funktionieren auch ohne Compiler. |

Die Projektvorlage unterstützt die Visual-Studio-Konfigurationen Debug und Release. Beim Bauen über MSBuild verwendet sie die Eigenschaft `BoltCompiler`, die Umgebungsvariable `BOLT_COMPILER` oder den Befehl `bolt` aus dem Suchpfad. Die Bolt-Menübefehle verwenden den in der Erweiterung gewählten Compiler.

Nach **Erstellen** enthält das Projekt `bolt.proj` und `src/Main.bolt`. Das Startprogramm gibt mit **Strg+F5** „Hello from Bolt!“ aus.

## Bauen und prüfen

In PowerShell im Quellbaum ausführen.

```powershell
.\editors\visualstudio\build.ps1
node --test .\editors\visualstudio\test-adapter.cjs
```

Der Build benötigt MSBuild aus Visual Studio, das .NET-Framework-4.8-Targeting-Pack und die in der Projektdatei festgelegten NuGet-Pakete. Das Skript verwendet die mit Visual Studio installierte Node.js-Distribution. Mit `-NodeDirectory` kann eine Windows-x64-Distribution ab Node.js 18 mit `node.exe` und `LICENSE` angegeben werden. Beim ersten Build installiert `npm ci` bei Bedarf die Abhängigkeiten aus dem Lockfile des VS-Code-Editors. Auf dem Zielrechner bringt die VSIX ihre JavaScript-Laufzeit mit.

Der Adaptertest verwendet ein temporäres Projekt und den echten Compiler. Der Hosttest benötigt Python mit pywin32 und eine in einem eigenen Visual-Studio-Testprofil installierte VSIX. Beispiel für das Profil `BoltTest` in Visual Studio 2022 Community.

```powershell
python .\editors\visualstudio\test-host.py --suffix BoltTest --devenv "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\devenv.exe" --bolt .\bin\bolt.exe --log "$env:TEMP\BoltTest.xml"
```

Mit `--template` erstellt der Hosttest ein Projekt aus dem Vorlagenkatalog, baut es in Debug und Release und prüft die Konsolenausgabe.
