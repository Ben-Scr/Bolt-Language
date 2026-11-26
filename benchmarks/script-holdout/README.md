# Unabhängige Skript-Stichprobe v1

Diese drei Aufgaben wurden nach Abschluss der Optimierungen für die erste Skript-Baseline festgelegt. Sie verwenden deterministische Beispieldaten und dieselben Zeitgrenzen für die Compilervergleiche. Die Ausgaben werden nach jeder Messung mit einer separaten Python-Referenz verglichen.

| Aufgabe | Voller Datensatz und Ergebnis | Änderung im warmen Host |
|---|---|---|
| Telemetrie | 48.000 numerische CSV-Zeilen mit Dienst-ID, HTTP-Status und Latenz einlesen. Pro Dienst die Anzahl der Anfragen, Serverfehler und langsamen Antworten ausgeben. | Die Latenzgrenze von einschließlich 250 ms auf strikt über 250 ms ändern. |
| Sprite-Atlas | 512 × 512 indizierte Pixel aus gemischten Wiederholungs- und Literalpaketen dekodieren. Eine RGB-Palette in einen RGBA-Puffer von 1 MiB expandieren. | Den transparenten Palettenindex von 7 auf 8 ändern. |
| Ereignis-Zeitleiste | 24.577 Ereignisse mit wiederkehrenden Zeitstempeln per iterativem Mergesort ordnen. Die Datei enthält die ursprünglichen Ereignisindizes in Zeitreihenfolge. | Bei gleichem Zeitstempel dem rechten statt dem linken Teillauf Vorrang geben. |

## Ausführen

In einer Windows-x64-Entwicklerkonsole mit MSVC, Clang, Python und .NET 10 im Bolt-Arbeitsverzeichnis ausführen. Das optionale Argument `--previous` erwartet die CLI eines vollständigen zweiten Bolt-SDKs.

```powershell
python benchmarks/script-holdout/compare_holdout.py --bolt bin/bolt.exe --previous C:/SDK-alt/bin/bolt.exe --rounds 36 --json benchmarks/results/script-holdout-v1-windows-x64.json
```

Die Release- und frischen Compilerläufe wechseln die Werkzeuge je Runde in balancierter Reihenfolge ab. Für die laufenden Hosts läuft jede Bolt-Version zusammen mit Roslyn in einem eigenen Prozess. Beide Compiler wechseln zwischen den zwei Quellständen. Der Runner prüft jede Revision nach der Messung auf kleineren Eingaben mit 64 CSV-Zeilen, 256 Pixeln oder 65 Ereignissen. Die frischen Debug-Builds und die Release-Builds durchlaufen jeweils die volle Eingabe.

Die Ende-zu-Ende-Laufzeit umfasst Prozessstart, Einlesen, Verarbeitung und Schreiben. Eine zweite, instrumentierte EXE misst die Verarbeitung unmittelbar nach dem Einlesen bis unmittelbar vor dem Schreiben, einschließlich Ausgabeallokation. Die Windows-Zeitquellen entsprechen denen der ersten Skript-Baseline. Der frische Bolt-Debug-Build erzeugt eine native EXE, Roslyn emittiert IL und portable PDB. Alle Messarten verwerfen vier Aufwärmrunden. Die JSON-Berichte enthalten Einzelwerte, Reihenfolgen, Prüfsummen und Compileroptionen.

Das frühere Vergleichs-SDK stammt aus dem F5-Benchmark vom 20. November 2026 um 15:42 Uhr. Beide getesteten SDKs melden Bolt 0.16.0, ihre Binärhashes stehen im Bericht. Der Vergleich umfasst Änderungen am Compiler, an der Laufzeit und an der SDK-Bauweise. Die drei Hashes für CLI, Compilerhost und Runtime des exakten Ausgangsstands der ersten Skript-Baseline weichen von denen des älteren Vergleichs-SDKs ab.
