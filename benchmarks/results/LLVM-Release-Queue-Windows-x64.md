# Queue in Bolt LLVM Release auf Windows x64

Die Messung vergleicht zwei aus demselben Quellstand gebaute Bolt-Compiler. Nur die Erkennung und Übersetzung numerischer Queue-Schleifen unterscheidet die Builds. Die SHA-256-Prüfsumme der AOT-Laufzeitbibliothek stimmt überein. Im Programmeinstieg der gemessenen Nachher-Binärdatei stehen für die innere Schleife jetzt unter anderem die SIMD-Instruktionen `pslld`, `psrld` und `paddq`. Die gemessenen Eingaben und alle C-Fassungen entsprechen dem [früheren C-Vergleich](LLVM-Release-vs-C-Windows-x64.md).

## Queue und Gesamtlaufzeit

Alle Werte sind Mediane. Die Gesamtzeit ist die Summe aller 33 Prozesslaufzeiten innerhalb einer Runde. Die letzte Spalte gibt die Bolt-Ersparnis gegenüber dem Vorher-Build mit gepaartem 95-%-Bootstrap-Intervall an. In den Queue-Zeilen ist die spezialisierte C-Fassung der Ringpuffer.

| Messung | Programm | Einheit | Bolt vorher | Bolt nachher | C regulär | C spezialisiert | Bolt-Ersparnis [95-%-Intervall] |
|---|---|---|---:|---:|---:|---:|---:|
| Windows-Planung | queue-worklist-long | ms | 186,54 | 63,27 | 129,81 | 73,05 | 66,1 % [64,8 % bis 76,3 %] |
| Windows-Planung | queue-worklist | ms | 19,39 | 11,76 | 15,89 | 11,93 | 39,4 % [37,5 % bis 50,3 %] |
| Windows-Planung | Alle 33 | s | 3,768 | 3,520 | 5,242 | 3,440 | 6,6 % [0,8 % bis 11,7 %] |
| Logischer Prozessor 8 | queue-worklist-long | ms | 186,99 | 60,58 | 126,54 | 71,43 | 67,6 % [67,4 % bis 67,9 %] |
| Logischer Prozessor 8 | queue-worklist | ms | 19,31 | 11,65 | 15,06 | 11,42 | 39,6 % [31,4 % bis 44,4 %] |
| Logischer Prozessor 8 | Alle 33 | s | 3,375 | 3,226 | 4,937 | 3,255 | 4,4 % [3,5 % bis 5,2 %] |

- Bei freier Windows-Planung lag der lange Queue-Test vorher 113,5 ms über dem C-Ringpuffer, nachher 9,8 ms darunter. Im direkten Vorher-nachher-Vergleich jeder Messrunde betrug die mediane Einsparung aller Programme 151,4 ms. Auf die beiden Queue-Programme entfielen im Median 132,4 ms. Die übrigen 31 zusammen benötigten 3,562 → 3,448 s (95-%-Intervall der Ersparnis -4,8 % bis 8,8 %).
- Auf logischem Prozessor 8 lag der lange Queue-Test vorher 115,6 ms über dem C-Ringpuffer, nachher 10,9 ms darunter. Im direkten Vorher-nachher-Vergleich jeder Messrunde betrug die mediane Einsparung aller Programme 140,7 ms. Auf die beiden Queue-Programme entfielen im Median 134,0 ms. Die übrigen 31 zusammen benötigten 3,168 → 3,154 s (95-%-Intervall der Ersparnis -0,5 % bis 1,1 %).

## Was sich am erzeugten Code ändert

Die Standard-Queue für numerische Elemente liest einen zusammenhängenden Bereich im Ausgabepuffer und hängt neue Elemente an den Eingabepuffer an. LLVM Release erkennt gezählte Schleifen mit einer Entnahme, einer Einfügung in dieselbe lokale Queue und dazwischenliegenden skalaren Berechnungen. Die Blocklänge ist durch verfügbare Elemente, freie Eingabeplätze und verbleibende Iterationen begrenzt. Dadurch gelten die Grenzprüfungen für den ganzen Block. Beim Nachfüllen oder Wachsen wird eine normale Queue-Iteration ausgeführt. Tritt ein Rechenfehler auf, schreibt der Compiler die bis dahin erfolgten Entnahmen und Einfügungen in den sichtbaren Zustand zurück. [LLVM-Lowering](../../src/native/QueueWorkLoop.inc)

Der lange Test startet mit 4.096 Einträgen und führt 120 Millionen Entnahme-Einfüge-Paare aus. Der C-Ringpuffer nutzt einen maskierten Index. Bolt kann innerhalb jeder zusammenhängenden Charge mehrere Werte parallel verarbeiten. Der Vorteil gilt für diese Zugriffsschleife. Beim kurzen Test umfasst dieselbe Berechnung acht Millionen Paare. [Bolt-Programm](../performance/cases/queue-worklist.bolt) · [C-Programme](../performance/cases/queue-worklist.c)

## Alle 33 Programme

Mediane in Millisekunden bei freier Windows-Planung. Für 13 Programme wurde eine vorab ausgewählte zweite C-Fassung gemessen. Die übrigen 20 verwenden in beiden C-Summen denselben Messwert.

| Programm | Bolt vorher | Bolt nachher | Nachher minus vorher | C regulär | C spezialisiert |
|---|---:|---:|---:|---:|---:|
| `queue-worklist-long` | 186,54 | 63,27 | -123,3 | 129,81 | 73,05 |
| `queue-worklist` | 19,39 | 11,76 | -7,6 | 15,89 | 11,93 |
| `arithmetic` | 421,59 | 422,49 | +0,9 | 404,12 | 404,12 |
| `buffer` | 8,04 | 7,76 | -0,3 | 8,89 | 7,33 |
| `buffer-long` | 137,59 | 144,93 | +7,3 | 279,52 | 15,80 |
| `buffer-record-add` | 10,50 | 9,70 | -0,8 | 7,65 | 7,65 |
| `buffer-truncate` | 11,25 | 10,69 | -0,6 | 10,08 | 10,08 |
| `buffer-truncate-long` | 147,78 | 149,16 | +1,4 | 144,67 | 144,67 |
| `dictionary-string` | 18,47 | 18,79 | +0,3 | 34,89 | 16,48 |
| `dictionary-string-long` | 277,55 | 286,52 | +9,0 | 886,57 | 267,63 |
| `float-parse` | 96,85 | 98,64 | +1,8 | 151,97 | 151,97 |
| `fp-integrate` | 157,45 | 156,66 | -0,8 | 156,84 | 156,84 |
| `indirect-array` | 135,22 | 138,27 | +3,1 | 149,57 | 148,92 |
| `list-edit` | 13,99 | 14,27 | +0,3 | 22,67 | 22,67 |
| `list-edit-long` | 69,17 | 68,18 | -1,0 | 186,16 | 186,16 |
| `list-fill-clear` | 8,45 | 8,19 | -0,3 | 7,95 | 7,95 |
| `list-search` | 8,61 | 8,31 | -0,3 | 10,14 | 9,53 |
| `list-search-long` | 91,88 | 93,61 | +1,7 | 394,44 | 131,51 |
| `list-search-u64` | 174,51 | 174,54 | +0,0 | 290,24 | 290,24 |
| `memory` | 80,66 | 80,90 | +0,2 | 102,31 | 102,31 |
| `memory-long` | 585,14 | 664,42 | +79,3 | 767,18 | 412,53 |
| `pointers` | 95,62 | 95,20 | -0,4 | 95,99 | 157,13 |
| `string-affixes` | 9,93 | 10,01 | +0,1 | 9,66 | 9,66 |
| `string-equality` | 29,99 | 29,91 | -0,1 | 25,97 | 25,97 |
| `struct-buffer` | 12,46 | 11,68 | -0,8 | 11,24 | 11,24 |
| `struct-copy-small` | 9,00 | 9,05 | +0,1 | 8,51 | 8,51 |
| `struct-copy-wide` | 11,07 | 10,68 | -0,4 | 10,76 | 10,76 |
| `struct-values` | 82,19 | 82,39 | +0,2 | 82,44 | 82,44 |
| `struct-zero` | 148,42 | 146,29 | -2,1 | 147,56 | 147,56 |
| `utf8-index` | 8,46 | 8,83 | +0,4 | 8,61 | 8,61 |
| `utf8-index-long` | 199,60 | 201,59 | +2,0 | 198,81 | 198,81 |
| `utf8-roundtrip` | 36,41 | 36,78 | +0,4 | 26,95 | 12,15 |
| `utf8-roundtrip-long` | 149,53 | 153,00 | +3,5 | 356,35 | 72,24 |

## Messung und Prüfung

AMD Ryzen 9 7900X, Windows 11 x64, Bolt LLVM Release und Clang 23.1.2. Clang übersetzte C11 mit `-O3 -DNDEBUG -march=native -ffp-contract=off`. Jeder Start umfasste den vollständigen Prozess einschließlich Ausgabe. Jede Ausgabe wurde gegen den Sollwert der Suite geprüft. Übersetzt wurde vor den Messrunden. Beide Serien verwendeten dieselben Binärdateien, jeweils zwei Aufwärmrunden, zufällige Programmreihenfolge und balancierte Variantenreihenfolgen. Die erste Serie umfasst 17 Messrunden bei freier Planung, die zweite 15 auf logischem Prozessor 8. Die 95-%-Intervalle stammen aus 4.000 Ziehungen vollständiger Messpaare. Schwankungen zwischen den Runden und Startreihenfolgen sind in den Rohdaten enthalten.

Der neue Regressionstest prüft unter anderem Fehler vor und nach dem Einfügen, Iteratorinvalidierung, negative Schleifenstarts und veränderliche Grenzen. Hinzu kommen die vorhandenen Tests für FIFO-Reihenfolge, Kopien, verwaltete Elemente und deren Lebensdauer sowie Differentialtests gegen Python `collections.deque` in Debug und Release. Der Regressionstest bestand in allen sechs geprüften JIT- und AOT-Modi und im Vergleich von Interpreter und C++-Backend. Weitere 76 Queue-Szenarien mit unterschiedlichen Startbelegungen, Kapazitäten und Schleifengrenzen lieferten in beiden LLVM-Release-Builds dieselben 930 erwarteten Ausgabezeilen. [Regressionstest](../../tests/native/queue-work-loop.bolt) · [Differentialtest](../../tests/queue_differential.py)

[Rohdaten mit allen Einzelstarts, Prüfsummen und Intervallen](llvm-release-queue-windows-x64.json) · [Befehl zur Reproduktion](../performance/README.md#queue-optimierung-mit-zwei-bolt-builds-und-c-messen)
