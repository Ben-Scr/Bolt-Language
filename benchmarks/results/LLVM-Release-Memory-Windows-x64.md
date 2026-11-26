# Memory in Bolt LLVM Release auf Windows x64

## Laufzeit vollständiger Programme

Je Zeile steht der Median aus 15 Messrunden. Für die Gesamtlaufzeit werden zuerst alle 33 Prozesszeiten pro Runde addiert. Beim langen Memory-Test vergleicht das reguläre C den ganzen Bereich mit `memcmp`. Die zweite C-Fassung vergleicht nach der Kopie nur das bekannte geänderte Byte. Beide führen Füllen, Kopieren und Verschieben aus. [C-Referenz](../performance/cases/memory.c) · [Bolt-Programm](../performance/cases/memory.bolt)

| Messung | Programm | Einheit | Bolt vorher | Bolt nachher | C regulär | C spezialisiert | Bolt-Ersparnis [95-%-Intervall] |
|---|---|---|---:|---:|---:|---:|---:|
| Windows-Planung | memory-long | ms | 581,26 | 236,11 | 739,40 | 402,37 | 59,4 % [58,2 bis 60,0 %] |
| Windows-Planung | memory | ms | 78,50 | 37,70 | 98,72 | 98,72 | 52,0 % [51,2 bis 53,3 %] |
| Windows-Planung | Alle 33 | s | 3,273 | 2,873 | 5,042 | 3,288 | 12,2 % [11,2 bis 13,6 %] |
| Logischer Prozessor 8 | memory-long | ms | 582,85 | 241,08 | 763,07 | 411,05 | 58,6 % [56,3 bis 64,1 %] |
| Logischer Prozessor 8 | memory | ms | 82,22 | 38,92 | 102,06 | 102,06 | 52,7 % [49,5 bis 54,9 %] |
| Logischer Prozessor 8 | Alle 33 | s | 3,688 | 3,086 | 5,257 | 3,525 | 16,3 % [7,4 bis 29,3 %] |

- Bei freier Windows-Planung betrug die Ersparnis aller 33 Programme im paarweisen Rundenvergleich im Median 389,3 ms. Die beiden Memory-Programme allein sparten 381,7 ms. Für die übrigen 31 zusammen lag das gepaarte 95-%-Intervall der Ersparnis bei -0,6 bis 2,1 %.
- Bei fester Prozessoraffinität betrug die Ersparnis aller 33 Programme im paarweisen Rundenvergleich im Median 490,3 ms. Die beiden Memory-Programme allein sparten 389,8 ms. Für die übrigen 31 zusammen lag das gepaarte 95-%-Intervall der Ersparnis bei -7,6 bis 20,9 %.

## Wo die Zeit in memory-long anfällt

Der Windows-Sampler unterbrach den Hauptthread in einem nominellen Intervall von 1 ms. Für zwölf zusätzliche Starts pro Build wurden Rücksprungadressen über die x64-Unwind-Daten ermittelt und den COFF-Aufrufen im Programmeinstieg zugeordnet. Die Profile stammen von denselben Binärdateien wie die Zeitmessung. Dadurch lassen sich Kopieren und Verschieben trotz identischer CRT-Einsprungadresse unterscheiden. Die Prozentwerte sind Stichprobenanteile des jeweiligen Builds.

| Ausgeführte Speicherprimitive | Stichproben vorher | Anteil vorher | Stichproben nachher | Anteil nachher |
|---|---:|---:|---:|---:|
| Kopieren | 2627 | 44,8 % | 0 | 0,0 % |
| Verschieben | 2136 | 36,4 % | 0 | 0,0 % |
| Füllen | 1000 | 17,0 % | 2399 | 94,7 % |
| Vergleichen | 58 | 1,0 % | 52 | 2,1 % |
| Übriger Code, einschließlich eingebetteter Instruktionen | 45 | 0,8 % | 81 | 3,2 % |

Vorher wurden 5.866 Stichproben aufgezeichnet, nachher 2.532. Von den 2.399 Stichproben beim Füllen im optimierten Programm entfielen 1.083 auf die Quelle und 1.316 auf das Ziel der Kopie. Die direkten Bytezugriffe beim Verschieben liegen im übrigen Code.

## Änderungen am Compiler

- Folgt eine Kopie auf das Füllen ihres Quellbereichs, prüft der optimierte Code die tatsächlich ausgewerteten Adressen und Längen. Bei gültigen, getrennten Bereichen kann Bolt das Ziel mit dem bekannten Byte füllen. Der Quellbereich muss dafür kein zweites Mal gelesen werden.
- Beim Verschieben eines nachweislich gleichmäßig gefüllten Bereichs behalten die Bytes im Überlappungsbereich ihren Wert. Bolt füllt nur den neu abgedeckten Rand. Eine zwischenzeitlich geänderte Bytestelle wird an ihrer alten Position bei Bedarf zurückgesetzt und am richtigen Zieloffset geschrieben. Bei unbekanntem Inhalt, anderen Quelladressen, einer größeren Länge oder ungültigen Adressen verwendet der Compiler den regulären Aufruf. [Implementierung](../../src/native/lowering/StorageOptimizations.cpp)
- Der lange Test führt zwei Millionen Durchgänge mit Längen von 1 bis 16.384 Bytes aus. Pro Speicheroperation summiert sich die angeforderte Länge auf 16.376.226.368 Bytes. Die Verschiebung um ein Byte erfordert hier nur einen neuen Randplatz und die Korrektur der geänderten Stelle. Anwendungen mit anderen Datenmustern können andere Ergebnisse liefern.

## Wirkung der beiden Änderungen für sich

In einer zusätzlichen Serie auf Prozessor 8 wurde jeweils nur einer der beiden neuen Pfade aktiviert. Die vier Varianten liefen pro Messrunde in wechselnder Reihenfolge. Die Mediane stammen aus 13 Messrunden.

| Build für memory-long | Median |
|---|---:|
| Ausgangsstand | 582,45 ms |
| Nur Kopieren aus einem gefüllten Bereich ersetzen | 434,46 ms |
| Nur Verschieben im bekannten Bereich verkürzen | 380,60 ms |
| Beide Änderungen | 238,32 ms |

## Alle 33 Programme

Mediane bei freier Windows-Planung in Millisekunden. Die spezialisierte C-Summe enthält die 13 vorab festgelegten C-Alternativen. Für die übrigen 20 Programme verwenden beide C-Summen dieselbe Messung.

| Programm | Bolt vorher | Bolt nachher | C regulär | C spezialisiert |
|---|---:|---:|---:|---:|
| `memory-long` | 581,26 | 236,11 | 739,40 | 402,37 |
| `memory` | 78,50 | 37,70 | 98,72 | 98,72 |
| `arithmetic` | 412,11 | 413,13 | 395,18 | 395,18 |
| `buffer` | 8,27 | 8,36 | 9,14 | 7,76 |
| `buffer-long` | 128,17 | 126,50 | 258,86 | 16,27 |
| `buffer-record-add` | 10,70 | 11,01 | 8,78 | 8,78 |
| `buffer-truncate` | 10,45 | 9,86 | 10,17 | 10,17 |
| `buffer-truncate-long` | 146,61 | 146,59 | 140,23 | 140,23 |
| `dictionary-string` | 17,54 | 17,82 | 38,11 | 17,38 |
| `dictionary-string-long` | 268,88 | 269,96 | 885,76 | 276,53 |
| `float-parse` | 96,49 | 95,29 | 152,75 | 152,75 |
| `fp-integrate` | 154,99 | 155,35 | 155,04 | 155,04 |
| `indirect-array` | 134,81 | 134,42 | 143,47 | 144,61 |
| `list-edit` | 11,54 | 11,95 | 22,78 | 22,78 |
| `list-edit-long` | 49,20 | 49,73 | 186,74 | 186,74 |
| `list-fill-clear` | 8,07 | 8,05 | 8,35 | 8,35 |
| `list-search` | 8,35 | 8,31 | 10,29 | 9,76 |
| `list-search-long` | 90,23 | 90,39 | 385,48 | 126,18 |
| `list-search-u64` | 172,76 | 173,28 | 285,12 | 285,12 |
| `pointers` | 93,52 | 93,50 | 94,96 | 155,17 |
| `queue-worklist` | 12,15 | 12,36 | 16,64 | 12,73 |
| `queue-worklist-long` | 61,31 | 61,45 | 127,64 | 71,43 |
| `string-affixes` | 9,76 | 9,70 | 9,47 | 9,47 |
| `string-equality` | 29,29 | 29,33 | 24,86 | 24,86 |
| `struct-buffer` | 12,33 | 12,15 | 12,61 | 12,61 |
| `struct-copy-small` | 8,53 | 8,45 | 8,87 | 8,87 |
| `struct-copy-wide` | 10,61 | 10,73 | 11,27 | 11,27 |
| `struct-values` | 81,96 | 81,05 | 81,66 | 81,66 |
| `struct-zero` | 136,45 | 133,93 | 138,32 | 138,32 |
| `utf8-index` | 9,00 | 8,73 | 8,33 | 8,33 |
| `utf8-index-long` | 196,21 | 194,45 | 193,68 | 193,68 |
| `utf8-roundtrip` | 36,61 | 37,41 | 27,26 | 12,28 |
| `utf8-roundtrip-long` | 143,43 | 141,79 | 314,40 | 70,84 |

## Messbedingungen und Prüfung

Windows 11 x64, Ryzen 9 7900X mit 24 logischen Prozessoren, LLVM und Clang 23.1.2. Beide Bolt-Compiler stammen aus einem isolierten Quellstand einschließlich der Queue-Optimierung und verwenden eine byteidentische AOT-Laufzeitbibliothek. C wurde mit `-O3 -DNDEBUG -march=native -ffp-contract=off` als C11 übersetzt. Gemessen wurde der gesamte Prozess einschließlich Start und Ausgabe. Jede Ausgabe wurde mit dem Solltext geprüft. Übersetzung und Sampling fanden außerhalb der Zeitmessung statt. Beide Serien verwendeten zwei Aufwärmrunden, eine wechselnde Programmreihenfolge und balancierte Reihenfolgen der Varianten. Die Intervalle stammen aus 4.000 gepaarten Bootstrap-Ziehungen.

Der neue Regressionstest vergleicht den vollständigen Puffer mit einer byteweisen Referenz. Er umfasst 855 Kombinationen von Richtung, Länge, Quellbereich und geänderter Position sowie Null-Längen und Ausnahmen. Er und der bestehende Test für kopierte Speicherbereiche bestanden in sechs JIT- und AOT-Modi sowie im C++-Backend. Für den portablen Interpreter prüft der Test die erwartete Diagnose für die dort nicht verfügbare native Speicher-API. [Regressionstest](../../tests/native/filled-memory-transfer.bolt)

[Einzelzeiten und Prüfsummen](llvm-release-memory-windows-x64.json) · [Stackprofile und Teilmessungen](llvm-release-memory-profile-windows-x64.json) · [Reproduktion](../performance/README.md#memory-laufzeiten-und-aufrufprofile-auf-windows-x64)
