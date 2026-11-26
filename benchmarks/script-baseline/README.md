# Feste Skript-Baseline v1

Die drei eigenständigen Skripte bilden Aufgaben aus Spiel und Werkzeugen nach. Jedes liest eine Binärdatei und schreibt ein fachliches Ergebnis. Die Quellen liegen für Bolt, C# und C getrennt vor. Für künftige Vergleiche bleiben die v1-Eingaben, Ausgabeformate und Algorithmen fest. Eine fachliche Änderung bekommt eine neue Baseline-Version.

| Skript | Daten und Aufgabe | Geprüftes Ergebnis |
|---|---|---|
| Navigation | 256 Weganfragen auf einer begehbaren Karte mit 128 × 128 Feldern, Wänden, Türen und einer isolierten Zelle. Breitensuche in vier Richtungen. | Kürzeste Weglänge oder Unerreichbar-Markierung für jede Anfrage |
| Geländeimport | 2048 × 2048 Höhenwerte. Höhe und lokale Steigung bestimmen Wasser, Boden und Steilhang. | Materialklasse jedes Pixels einschließlich des Kartenrands |
| Warenbestand | 750.000 Zugänge und Entnahmen für 4.096 Artikel. Entnahmen über dem Bestand werden abgelehnt. | Endbestand und Anzahl abgelehnter Entnahmen für jeden Artikel |

Der Generator läuft außerhalb der Messung. Eine eigenständige Python-Referenz bestimmt jedes erwartete Ausgabebyte. SHA-256-Prüfsummen in `v1.json` fixieren die Eingaben und beide Ausgaben pro Aufgabe. Der Runner prüft jedes gestartete Programm und jeden kompilierten Editorstand.

CLI-Builds und Release-Programme verarbeiten die vollständigen Daten. Bei der Qualitätsprüfung nach der warmen Kompilierung erhalten beide Hosts dieselbe kleine Eingabe, damit die Ausführung in das Instruktionsbudget des eingebetteten Bolt-Debug-Hosts passt. Das kompilierte Skript hat weiterhin den vollen Umfang. Die Eingabe deckt beide Seiten der tatsächlichen Codeänderung ab.

## Auf Windows x64 wiederholen

Voraussetzungen sind die x64-Entwicklerkonsole von Visual Studio mit MSVC, Clang, Python, das .NET-10-SDK und das Bolt-SDK des Arbeitsverzeichnisses. Im Repository-Stamm ausführen:

```powershell
python benchmarks/script-baseline/run.py --json benchmarks/results/script-baseline-v1-windows-x64.json
python benchmarks/script-baseline/phases.py --json benchmarks/results/script-baseline-v1-phases-windows-x64.json
```

Über `--bolt` und `--clang` lassen sich andere Compilerpfade angeben. Die Standardeinstellung verwendet vier Aufwärmrunden und 18 Messrunden je Skript, Messart und Compiler. Die Reihenfolge wird mit festem Seed variiert und über die Messpositionen ausbalanciert. Die Ergebnisdatei enthält alle Zeiten, Mediane, p95, Reihenfolgen sowie Fingerabdrücke der Quellen und Werkzeuge.

Für die optimierte Windows-x64-Messung wurde das Bolt-SDK mit MSVC in der Konfiguration Release gebaut. Liegen `BoltAotRuntime.lib` neben dem Compiler sowie die x64-Linkbibliotheken von MSVC und Windows SDK vor, bindet Bolt die verwendeten Laufzeitfunktionen direkt in die Release-EXE ein. SDKs ohne diese Bibliotheken und Skripte mit dynamischer Bolt-Codeausführung verwenden `BoltRuntime.dll`. Mit `bolt compile --release --verbose` lässt sich die gewählte Variante an der Linkerzeile ablesen. Die längere Messreihe lässt sich mit `--rounds 60` wiederholen.

| Messart | Zeitgrenzen |
|---|---|
| Debug, frischer Prozess | Start der Compiler-CLI bis zu ihrem Ende, jeweils in einem frischen Buildverzeichnis mit warmem Dateisystemcache. Bolt Fast erzeugt eine native EXE, direkt aufgerufenes Roslyn eine Debug-IL-Assembly mit portabler PDB. Projekt-Restore und MSBuild sind außerhalb der Messung. |
| Debug, laufender Host | Eine geänderte Konstante im Methodenrumpf bis zum neuen Modul im Speicher. Bolt verwendet die Debug-Compilerhost-API, Roslyn ersetzt den geänderten Syntaxbaum und emittiert IL und portable PDB. Beide Versionen werden abwechselnd gebaut und nach der Messung ausgeführt. |
| Release-Laufzeit | Prozessstart bis Prozessende einschließlich Einlesen, Berechnung und Schreiben. Bolt verwendet LLVM, die beiden C-Builds verwenden MSVC `/O2` und Clang `-O3`. Die Ergebnisprüfung folgt außerhalb der Messung. |
| Leerer Prozess | Gleiches Release-Profil mit leerem Hauptprogramm. Dieser Kontrollwert zeigt die Größenordnung des Starts auf demselben Rechner. |

Der zweite Befehl baut temporäre Release-Kopien mit drei internen Zeitmessungen. Sie erfassen Einlesen, Verarbeitung einschließlich Ausgabeallokation sowie Schreiben. Diese Werte dienen der Ursachenanalyse. Die Ende-zu-Ende-Messung verwendet stets die ursprünglichen Skripte.

Die Skripte verwenden deterministische Beispieldaten. Die Zeiten gelten für einen vollständigen Job mit warmem Dateisystemcache. Der Editorwert umfasst den Compileranteil einer Änderung. Vorgänge wie Debugger-Anbindung, Engine-Reload, GPU-Arbeit oder ein erster gerenderter Frame benötigen eigene Messungen im jeweiligen Projekt.
