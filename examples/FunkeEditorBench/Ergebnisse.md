# Kompilierzeiten im C++-Editor-Simulator

Bolt kompilierte in diesem Versuch bei beiden Projektgrößen und allen drei Arten von Änderungen schneller als Funkes C#-Compiler. Jeder Messpunkt enthält 100 Änderungen in einem laufenden Editor. Die nachfolgenden Perzentile geben Millisekunden an.

## Zeit für die Kompilierung

| Änderung | Dateien | Bolt p50 / p95 | C# p50 / p95 | C# ohne Symbole p50 / p95 | C#-Zeit / Bolt-Zeit im Median¹ |
|---|---:|---:|---:|---:|---:|
| Zahlenwert | 2 | 1,42 / 1,83 | 11,00 / 22,04 | 11,49 / 18,87 | 7,76× |
| Zahlenwert | 32 | 4,16 / 6,65 | 36,11 / 59,15 | 34,88 / 61,71 | 8,68× |
| Verzweigung und Operator | 2 | 1,85 / 2,50 | 12,95 / 19,75 | 14,52 / 22,93 | 6,99× |
| Verzweigung und Operator | 32 | 4,40 / 7,57 | 39,47 / 60,16 | 37,34 / 63,99 | 8,97× |
| Methodensignatur und Aufruf | 2 | 1,75 / 2,25 | 12,66 / 19,40 | 13,53 / 22,05 | 7,24× |
| Methodensignatur und Aufruf | 32 | 3,90 / 6,17 | 35,05 / 54,05 | 30,16 / 50,34 | 8,99× |

¹ Quotient der beiden Mediane für das C#-Standardprofil und Bolt.

Von 600 direkt gepaarten Einzeländerungen dauerte die Bolt-Kompilierung in 0 Fällen länger als beim C#-Standardprofil und in 0 Fällen länger als bei C# ohne Symbole.

Für den Vergleich mit dem früheren Compiler wurde dieselbe Änderung eines Zahlenwerts auf derselben Maschine gemessen. Die beiden Messreihen liefen zu unterschiedlichen Zeitpunkten.

| Dateien | Bolt zuvor, p50 | Bolt aktuell, p50 |
|---:|---:|---:|
| 2 | 37,20 ms | 1,42 ms |
| 32 | 43,40 ms | 4,16 ms |

## Zeit bis zum ersten Frame mit der Änderung

| Änderung | Dateien | Bolt p50 / p95 | C# p50 / p95 | C# ohne Symbole p50 / p95 |
|---|---:|---:|---:|---:|
| Zahlenwert | 2 | 1,47 / 1,91 | 11,88 / 28,75 | 12,31 / 19,63 |
| Zahlenwert | 32 | 4,34 / 6,87 | 37,93 / 60,56 | 36,94 / 64,80 |
| Verzweigung und Operator | 2 | 1,93 / 2,64 | 13,85 / 21,56 | 15,52 / 23,95 |
| Verzweigung und Operator | 32 | 4,59 / 7,77 | 41,55 / 63,10 | 39,47 / 65,71 |
| Methodensignatur und Aufruf | 2 | 1,82 / 2,34 | 13,62 / 20,34 | 14,55 / 22,89 |
| Methodensignatur und Aufruf | 32 | 4,04 / 6,34 | 37,28 / 55,41 | 32,22 / 51,90 |

## Aktivierung des neuen Moduls

| Änderung | Dateien | Bolt p50 / p95 | C# p50 / p95 | C# ohne Symbole p50 / p95 |
|---|---:|---:|---:|---:|
| Zahlenwert | 2 | 0,04 / 0,06 | 0,33 / 0,63 | 0,36 / 0,52 |
| Zahlenwert | 32 | 0,07 / 0,15 | 0,41 / 0,56 | 0,44 / 0,74 |
| Verzweigung und Operator | 2 | 0,05 / 0,08 | 0,38 / 0,54 | 0,39 / 0,62 |
| Verzweigung und Operator | 32 | 0,07 / 0,14 | 0,44 / 0,67 | 0,44 / 1,17 |
| Methodensignatur und Aufruf | 2 | 0,04 / 0,08 | 0,36 / 0,50 | 0,40 / 0,66 |
| Methodensignatur und Aufruf | 32 | 0,05 / 0,08 | 0,41 / 0,65 | 0,40 / 0,64 |

## Erster Bewegungstick nach der Modulaktivierung

| Änderung | Dateien | Bolt p50 / p95 | C# p50 / p95 | C# ohne Symbole p50 / p95 |
|---|---:|---:|---:|---:|
| Zahlenwert | 2 | 0,01 / 0,02 | 0,47 / 0,87 | 0,46 / 0,86 |
| Zahlenwert | 32 | 0,09 / 0,13 | 1,52 / 2,31 | 1,54 / 2,75 |
| Verzweigung und Operator | 2 | 0,01 / 0,02 | 0,58 / 0,85 | 0,58 / 0,78 |
| Verzweigung und Operator | 32 | 0,09 / 0,16 | 1,76 / 2,60 | 1,69 / 2,48 |
| Methodensignatur und Aufruf | 2 | 0,01 / 0,03 | 0,56 / 0,74 | 0,58 / 0,84 |
| Methodensignatur und Aufruf | 32 | 0,09 / 0,15 | 1,36 / 2,52 | 1,39 / 2,28 |

## Warmstart und erster Prozessstart

Der C++-Editor hält 128 Entity-Transforms im Speicher. Ein Warmwert für die Kompilierung endet mit der Rückgabe des neuen Moduls. Die zweite Tabelle schließt dessen Aktivierung und den ersten Bewegungstick ein. Das Speichern der Datei geschieht vor der Zeitmessung. Der erste Start schließt Prozessstart, Initialisierung und Kompilierung bis zur ersten geprüften Bewegung ein. Pro Profil, Projektgröße und Editvariante gibt es einen solchen Start.

| Änderung | Dateien | Bolt erster Start | C# erster Start | C# ohne Symbole erster Start |
|---|---:|---:|---:|---:|
| Zahlenwert | 2 | 166,94 ms | 2863,76 ms | 3006,65 ms |
| Zahlenwert | 32 | 126,03 ms | 3210,73 ms | 3276,67 ms |
| Verzweigung und Operator | 2 | 96,86 ms | 3546,17 ms | 3449,68 ms |
| Verzweigung und Operator | 32 | 131,75 ms | 3838,43 ms | 2584,22 ms |
| Methodensignatur und Aufruf | 2 | 67,68 ms | 3149,12 ms | 3071,07 ms |
| Methodensignatur und Aufruf | 32 | 175,30 ms | 2589,82 ms | 2781,30 ms |

## Umfang und Reproduktion

Debian 13, Linux x86-64, AMD EPYC 9V74 auf einer gemeinsam genutzten Maschine. Alle Editorprozesse und ihre Kinder waren an logische CPU 7 gebunden. Nach acht Aufwärmrunden liefen 100 Messrunden mit wechselnder Reihenfolge der drei Compilerprofile. Der C++-Editor verwendet Bolts nativen Fast-Debug-JIT und Funkes Roslyn-ScriptCompiler aus Commit `67e570bf2940b975185d0f000aba01aafe6ffbd6` mit .NET SDK 10.0.404 und CoreCLR 10.0.15. Die C#-Variante ohne Symbole verwendet dieselben Compilerquellen.

Die 1.800 gemessenen Änderungen und 144 Aufwärmänderungen erzeugten die erwarteten Szenenprüfsummen. Nach jedem der 18 absichtlich fehlerhaften Edits lief die vorherige Generation weiter. Eine anschließende Reparatur aktivierte die neue Bewegung.

Der inkrementelle Bolt-Pfad prüft neue Methodenrümpfe vollständig gegen die vorhandenen Deklarationen. Bei Änderungen an skalaren Parametern bindet er alle betroffenen Projektklassen und ihre Aufrufer erneut. Die Messprojekte enthalten statische Methoden, skalare Werte und Aufrufe nativer Engine-Funktionen. Andere Editarten können eine vollständige semantische Analyse auslösen, etwa Änderungen an Importen, Klassen, Generics oder Heap-Datenstrukturen. Messwerte für diese Pfade und weitere Zielsysteme sind offen.

Für neue Rohdaten die drei Editarten nacheinander ausführen. Das Beispiel verwendet Bash, die lokale .NET-10-Installation und die im SDK bereitgestellte Bolt-Compilerbibliothek.

```sh
for edit in literal body declaration; do
  taskset -c 7 python3 compare.py --languages bolt csharp csharp-nosymbols \
    --files 2 32 --runs 100 --warmup 8 --seed 62201 --edit "$edit" \
    --json "messung-$edit.json"
done
```

[Zahlenwert](vergleich-linux-x64.json) · [Verzweigung und Operator](vergleich-linux-x64-body.json) · [Methodensignatur und Aufruf](vergleich-linux-x64-declaration.json) · [Vorherige Messung](vergleich-vor-inkrementell-linux-x64.json) · [Szenario und Bedienung](README.md)
