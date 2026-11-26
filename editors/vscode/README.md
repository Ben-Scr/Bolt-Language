# Bolt für Visual Studio Code

Die Erweiterung öffnet `.bolt`-Dateien, zeigt Compilerfehler beim Tippen an und startet Programme über das grüne Dreieck rechts oben im Editor. Sie verwendet den Language Server und die Buildprofile des installierten Bolt-SDK.

Bolt-Dateien unterstützen Quellzeilen-Haltepunkte im Editor. Für eine native Debugsitzung unter Linux wird ein GDB-kompatibler VS-Code-Debugadapter benötigt.

Bei Versions- oder Farbfragen öffnet **Bolt: Show Toolchain Info** den Bolt-Ausgabekanal. Dort stehen Erweiterungsversion, Pfad und Version des tatsächlich aufgerufenen Compilers, aktives Farbschema und die für Bolt wirksame Einstellung für semantisches Highlighting. Bolt 0.16 meldet außerdem Zielarchitektur und Runtime-ABI.

## Installieren

Die fertigen SDK-ZIPs enthalten die Erweiterung. Im entpackten Windows-SDK `.\setup.cmd` ausführen, im Linux-SDK `sh ./setup.sh`. Das Setup installiert die VSIX mit dem Kommando `code`, sofern dieses verfügbar ist. Nach einem vollständigen Neustart von VS Code kann die Erweiterung den neu eingerichteten Suchpfad verwenden.

Alternativ lässt sich die Erweiterung manuell installieren.

1. In VS Code **Extensions** öffnen, im Menü **…** den Eintrag **Install from VSIX…** wählen und `Bolt-VSCode.vsix` öffnen. Alternativ im Terminal ausführen.

   ```sh
   code --install-extension Bolt-VSCode.vsix
   ```

2. Den Ordner mit dem Bolt-Projekt öffnen. Steht `bolt` im Suchpfad, erkennt die Erweiterung den Compiler. Andernfalls über die Befehlspalette **Bolt: Select Compiler…** den absoluten Pfad zu `bolt.exe` unter Windows bzw. `bin/bolt` unter Linux oder macOS wählen. Für das entpackte Windows-SDK ist ein typischer Pfad `C:\Tools\Bolt-Windows-x64\bin\bolt.exe`. Bei SSH, WSL oder Dev Containers gehört das SDK auf den Rechner, auf dem die Workspace-Erweiterung läuft.
3. Eine `.bolt`-Datei öffnen. **▶** oder **Strg+F5** führt das zugehörige Projekt aus. Bei einer losen Datei baut Bolt diese Datei. Der Compiler, der Linker und die passenden Runtime-Bibliotheken kommen aus der lokalen SDK-Installation.

## Bearbeiten und ausführen

| Funktion | Bedienung |
|---|---|
| Syntaxfarben | Bolt-Dateien verwenden automatisch die 29 Farben des hellen oder dunklen IntelliJ-Bolt-Schemas. Der Language Server unterscheidet unter anderem Klassen, Felder, Parameter und inaktiven Code. Ohne SDK gelten die lexikalischen Farben des IntelliJ-Plug-ins, auch für interpolierte Strings. In Hochkontrast-Themes übernimmt VS Code die Darstellung. Über **Preferences: Color Theme** sind zusätzlich **Bolt C# Dark** und **Bolt C# Light** wählbar. |
| Diagnose und Navigation | Probleme-Ansicht, Hover, Vervollständigung, Signaturhilfe, F12 für Definition, Umschalt+F12 für Referenzen, F2 zum Umbenennen, Gliederung und Symbolsuche der Datei. Definitionen in der eingebetteten Standardbibliothek öffnen sich schreibgeschützt. |
| Ausführen und bauen | Grünes Dreieck im Editor, **Bolt: Run**, **Build**, **Check** sowie das Editor-Kontextmenü. In einem Projekt mit `bolt.proj` erscheint ein zusätzlicher Run-Link am vom Compiler bestätigten, gespeicherten Einstiegspunkt. |
| Arbeitsprofil | **Bolt: Select Build and Analysis Profile** wählt Projektstandard, Debug oder Release. Das Profil gilt für bedingte Codeanalyse und die Build-/Run-Befehle. **Restart Language Server** liest geänderte Einstellungen ein. |
| Generische Typen | Beim Tippen von `<` hinter einer aufgelösten generischen Deklaration ergänzt der Compilerclient die schließende Klammer. Ein Vergleich wie `a < b` behält sein einzelnes Zeichen. |
| Formatieren | **Format Document** oder Strg+K, Strg+D (macOS Cmd+K, Cmd+D). Allman-Klammern, eingerückte Blöcke und Zugriffsebenen. Mehrzeilige Blöcke sind über den Rand faltbar. |
| Klassen und Projekte | Im Ordner-Kontextmenü **Bolt: New Class…** bzw. **New Project…** oder über die Befehlspalette. Neue Projekte verwenden `bolt init`. |
| Terminal | Run und Build benutzen VS-Code-Prozesstasks. Interaktive Eingabe, Abbruch und Compilerdiagnosen werden über Terminal und Probleme-Ansicht verarbeitet. `bolt.run.arguments`, `bolt.run.environment` und `bolt.run.workingDirectory` steuern den Programmstart. |

In einem ungespeicherten Tab den Sprachmodus **Bolt** wählen. Hover, Definitionen und Umbenennen arbeiten dann mit dem aktuellen Text. Zwei neue Tabs werden als unabhängige Skripte analysiert. Nach dem Speichern im Quellordner eines Projekts gilt dessen Projektkontext.

Bei `Random.Next<int>(0, 5)` ohne `import Random` setzt der Compiler eine Diagnose. Am markierten Aufruf **Strg+.** und **Add import Random** wählen. Der Language Server kann bei einem fehlerhaften Edit weiterhin eindeutig erkennbare Deklarationen und sichtbare Variablen auflösen. Mit **F12** erreichbare Quellen der eingebetteten Standardbibliothek sind schreibgeschützt.

Über **Tasks: Run Task** bzw. **Tasks: Configure Default Build Task** lassen sich die Aktionen als normale VS-Code-Tasks verwenden. Beispiel für `.vscode/tasks.json` in einem Workspace mit zwei Projekten.

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "Spiel starten",
      "type": "bolt",
      "operation": "run",
      "target": "Game",
      "profile": "Release",
      "args": ["--scene", "Test Level"]
    }
  ]
}
```

Die lokale Syntaxdarstellung, Formatierung und Blockfaltung funktionieren auch ohne SDK. VS Codes Restricted Mode beschränkt die Erweiterung auf lokale Bearbeitung. Das Debug-Buildprofil legt die Compileroptionen fest. Für Breakpoints, Watches und Einzelschritte wäre ein eigener Debug-Adapter erforderlich.

## Erweiterung aus dem Quellcode bauen

Im Ordner `editors/vscode` mit Node.js 22 oder neuer ausführen.

```sh
npm ci
npm test
npm run package
```

Das VSIX enthält den gebündelten Language Client, die Grammatik und alle dafür nötigen JavaScript-Laufzeitbibliotheken. Der Hosttest in `test/host.cjs` verwendet eine installierte Extension und einen echten Compiler. `test/driver` ist ein leerer Development-Host für den VS-Code-Integrationstest.

Einen Integrationstest auf einem Rechner mit grafischem VS-Code-Desktop ausführen. Der Test verwendet ein temporäres Profil und ein eigenes kleines Bolt-Projekt.

```sh
python3 scripts/test-installed.py --code /pfad/zu/code --bolt /pfad/zu/bolt --vsix /pfad/zu/Bolt-VSCode.vsix
```
