# Bolt LLVM Release im Vergleich mit C auf Windows x64

Auf dem AMD Ryzen 9 7900X brauchten die 33 Programme mit regulärer Windows-Prozessorplanung im Median **3,365 s in Bolt**, **4,711 s mit den regulären C-Referenzen** und **3,084 s mit den spezialisierten C-Referenzen**. Die Bolt-Gesamtlaufzeit war gegenüber den regulären C-Referenzen 28,6 % kürzer, gegenüber den spezialisierten 9,1 % länger.

| Messung | Bolt | C regulär | C spezialisiert | Bolt gegenüber C regulär, 95-%-Intervall | Bolt gegenüber C spezialisiert, 95-%-Intervall |
|---|---:|---:|---:|---:|---:|
| Windows-Planung (15 Runden) | 3,365 s | 4,711 s | 3,084 s | -28,9 bis -28,4 % | 8,1 bis 9,6 % |
| Prozessor 8 (11 Runden) | 3,469 s | 4,844 s | 3,174 s | -29,1 bis -27,6 % | 8,5 bis 10,0 % |

Negative Prozentwerte stehen für kürzere Bolt-Laufzeiten. Der Nenner ist die jeweilige C-Laufzeit. Die Rundensumme zählt jedes der 33 Programme einmal. Bei 13 Programmen lief zusätzlich eine vorab ausgewählte spezialisierte C-Fassung. Die übrigen 20 verwenden in beiden C-Summen dieselbe Einzelmessung.

## Was die Gesamtlaufzeit bestimmt

Eine spätere Messung vergleicht beide Bolt-Builds nach der gezielten Queue-Optimierung mit denselben C-Fassungen. [Queue vorher und nachher über alle 33 Programme](LLVM-Release-Queue-Windows-x64.md)

- Gegenüber den regulären C-Referenzen lagen die größten Zeitgewinne von Bolt bei `dictionary-string-long` (531,0 ms), `list-search-long` (281,2 ms), `utf8-roundtrip-long` (166,6 ms). Die größten Mehrzeiten lagen bei `queue-worklist-long` (59,0 ms), `arithmetic` (20,7 ms), `utf8-roundtrip` (7,9 ms). Bei 13 Programmen lag das Einzelintervall vollständig auf der schnelleren Seite, bei 11 auf der langsameren. Bei 9 schloss es Gleichstand ein.
- Gegenüber den spezialisierten C-Referenzen lagen die größten Zeitgewinne von Bolt bei `list-search-u64` (107,3 ms), `pointers` (57,6 ms), `float-parse` (48,4 ms). Die größten Mehrzeiten lagen bei `memory-long` (164,2 ms), `queue-worklist-long` (113,0 ms), `buffer-long` (105,4 ms). Bei 8 Programmen lag das Einzelintervall vollständig auf der schnelleren Seite, bei 16 auf der langsameren. Bei 9 schloss es Gleichstand ein.

Die C-Variante für `float-parse` verwendet die Windows-C-Laufzeit mit `strtod` und `strtof`, Bolt seinen fast_float-Parser. Beide verarbeiten eine Million Zahlenpaare und prüfen über die Ausgabe die resultierenden Bitmuster. Beim langen Wörterbuchtest berechnet die reguläre C-Fassung den Hash pro Suche, die spezialisierte C-Fassung speichert ihn für die vorbereiteten Schlüssel. Beim UTF-8-Rundlauf nutzt die spezialisierte C-Fassung denselben SIMD-Validierungscode wie Bolt. [C-Zahlenparser](../performance/cases/float-parse.c) · [Hash-Suche](../performance/cases/dictionary-string.c) · [UTF-8-Rundlauf](../performance/cases/utf8-roundtrip.c)

## Alle 33 Programme

Median in Millisekunden bei regulärer Windows-Planung. Die Spalte „Bolt minus C“ bezieht sich auf die reguläre C-Referenz. Ein Strich in der letzten Spalte bedeutet, dass der reguläre C-Wert auch für die spezialisierte Gesamtsumme gilt.

| Programm | Bolt ms | C regulär ms | Bolt minus C ms | C spezialisiert ms | Spezialisierte C-Fassung |
|---|---:|---:|---:|---:|---|
| `dictionary-string-long` | 271,86 | 802,85 | -530,99 | 250,58 | `dictionary-string-long-cached` |
| `memory-long` | 553,60 | 718,75 | -165,14 | 389,42 | `memory-long-prefix` |
| `arithmetic` | 400,73 | 380,00 | 20,73 | 380,00 | – |
| `list-search-long` | 84,86 | 366,04 | -281,18 | 120,09 | `list-search-long-avx2` |
| `utf8-roundtrip-long` | 136,73 | 303,30 | -166,58 | 66,29 | `utf8-roundtrip-long-simd` |
| `list-search-u64` | 167,64 | 274,95 | -107,31 | 274,95 | – |
| `buffer-long` | 118,43 | 235,39 | -116,97 | 13,05 | `buffer-long-fused-unroll8` |
| `utf8-index-long` | 188,12 | 189,17 | -1,05 | 189,17 | – |
| `list-edit-long` | 183,97 | 177,88 | 6,09 | 177,88 | – |
| `queue-worklist-long` | 181,48 | 122,49 | 58,99 | 68,45 | `queue-worklist-long-ring` |
| `fp-integrate` | 148,97 | 149,39 | -0,42 | 149,39 | – |
| `pointers` | 89,38 | 89,56 | -0,18 | 146,94 | `pointers-mask` |
| `float-parse` | 97,80 | 146,17 | -48,37 | 146,17 | – |
| `buffer-truncate-long` | 139,68 | 135,59 | 4,10 | 135,59 | – |
| `indirect-array` | 129,73 | 135,76 | -6,03 | 139,00 | `indirect-array-unchecked` |
| `struct-zero` | 127,54 | 127,36 | 0,18 | 127,36 | – |
| `memory` | 73,62 | 94,83 | -21,21 | 94,83 | – |
| `struct-values` | 77,40 | 77,09 | 0,32 | 77,09 | – |
| `utf8-roundtrip` | 31,67 | 23,77 | 7,89 | 9,51 | `utf8-roundtrip-simd` |
| `dictionary-string` | 15,48 | 30,83 | -15,35 | 13,52 | `dictionary-string-cached` |
| `string-equality` | 28,62 | 21,80 | 6,83 | 21,80 | – |
| `list-edit` | 20,90 | 19,89 | 1,01 | 19,89 | – |
| `queue-worklist` | 17,68 | 13,06 | 4,62 | 9,47 | `queue-worklist-ring` |
| `struct-buffer` | 9,82 | 9,19 | 0,64 | 9,19 | – |
| `struct-copy-wide` | 8,15 | 8,64 | -0,50 | 8,64 | – |
| `buffer-record-add` | 8,39 | 5,86 | 2,53 | 5,86 | – |
| `list-search` | 6,18 | 8,19 | -2,01 | 7,19 | `list-search-simd-unroll4` |
| `list-fill-clear` | 7,96 | 5,62 | 2,34 | 5,62 | – |
| `buffer-truncate` | 7,87 | 7,73 | 0,14 | 7,73 | – |
| `string-affixes` | 7,26 | 6,98 | 0,28 | 6,98 | – |
| `utf8-index` | 6,61 | 5,92 | 0,68 | 5,92 | – |
| `buffer` | 5,64 | 6,44 | -0,81 | 5,40 | `buffer-fused` |
| `struct-copy-small` | 6,43 | 6,20 | 0,23 | 6,20 | – |

## Messbedingungen

Windows 11 x64, AMD Ryzen 9 7900X mit 24 logischen Prozessoren. Verwendet wurden die per SHA-256 verifizierten Bolt-Binärdateien des letzten LLVM-Release-Vergleichs. Clang 23.1.2 übersetzte die C11-Referenzen mit `-O3 -DNDEBUG -march=native -ffp-contract=off`. Das CPU-Ziel war `znver4`. Die ausgeschaltete FMA-Kontraktion erhält die Rundung der getrennten Rechenoperationen im Gleitkommatest. Für Bolt wurde der vorhandene Release-Build mit seiner regulären CPU-Auswahl verwendet.

Jede Variante jedes Programms lief als eigener Prozess. Erfasst wurde die Zeit ab dem Startaufruf bis zum Prozessende einschließlich der Ausgabe. Jedes Ergebnis wurde mit dem festen Solltext der Suite verglichen. Übersetzung und Profiling liegen außerhalb der Zeitmessung. Zwei Aufwärmrunden gingen jeder Serie voraus. Pro Runde wechselte die Reihenfolge der Programme nach einem festen Seed, die zwei oder drei ausführbaren Varianten je Programm belegten in vollständigen Blöcken jede Position gleich oft. Die zweite Serie lief mit denselben Binärdateien und einem anderen Seed auf einem fest zugewiesenen Prozessor.

Die 95-%-Intervalle basieren auf 4.000 Bootstrap-Ziehungen vollständiger Messpaare. Die Einzelintervalle gelten je Programm, ohne Korrektur für Mehrfachvergleiche. Die C-Referenzen liefern dieselben Sollausgaben. Einige spezialisierte Fassungen verwenden Wissen über die Eingaben oder den weiteren Programmablauf. Beispielsweise kann die fusionierte Buffer-Fassung Werte sofort summieren und die Speicher-Vergleichsfassung nach der Kopie nur das bekannte veränderte Byte prüfen. Die Zahlen gelten für diese Programme und Compiler auf diesem Rechner.

[Rohdaten mit Einzelzeiten, Reihenfolge, Flags und Prüfsummen](llvm-release-c-windows-x64.json) · [Reproduktion](../performance/README.md#llvm-release-mit-c-vergleichen) · [Vorher-nachher-Vergleich von Bolt](LLVM-Release-Gesamtlaufzeit-Windows-x64.md)
