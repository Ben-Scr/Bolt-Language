# LLVM Release auf Windows x64 – vollständige Programme

Mit normaler Windows-Prozessorplanung sank die Zeit für einen Durchlauf aller 33 Programme um **4,4 %**. Auf einem fest zugewiesenen Kern betrug die Änderung **4,4 %**. Jede Variante führte identische Bolt-Quellen mit denselben Sollausgaben aus. Beide Zeitreihen und die Profile verwenden dieselben 66 Binärdateien.

| Messung | Median der Rundensumme vorher → nachher | Einsparung | Gepaarter 95-%-Bereich | p95 vorher → nachher |
|---|---:|---:|---:|---:|
| Reguläre Windows-Planung, 11 Messrunden | 3,521 → 3,366 s | 4,4 % | 2,7 bis 5,0 % | 3,700 → 3,633 s |
| Logischer Prozessor 8, 21 Messrunden | 3,570 → 3,413 s | 4,4 % | 3,4 bis 5,1 % | 4,187 → 3,848 s |

Unter regulärer Prozessorplanung lagen vier der 33 Einzelintervalle vollständig auf der schnelleren Seite. Die übrigen 29 schlossen Gleichstand ein. Auf dem festen Kern dauerte `fp-integrate` zusätzlich 0,50 ms länger (+0,3 %). Unter regulärer Planung lagen beide Mediane bei 150,1 ms.

## Engpässe und Änderungen

Dieselben 33 Bolt-Binärdateien wurden auch mit den C-Referenzen verglichen. [Bolt LLVM Release im Vergleich mit C](LLVM-Release-vs-C-Windows-x64.md)

- **Dezimalzahlen.** Bei `float-parse` lagen vor der Änderung 32 % der Stichproben allein in den beiden MSVC-Routinen für die Umwandlung von Dezimalziffern. Der Windows-Build nutzt jetzt den vorhandenen fast_float-Parser direkt in der Zielpräzision. Das vollständige Programm brauchte 193,6 → 98,6 ms. [Dezimalkonvertierung](../../include/bolt/Decimal.hpp)
- **UTF-8.** Bei `utf8-roundtrip-long` entfielen zunächst 50 % der Stichproben auf die Blockvalidierung. Auch MSVC-Builds wählen jetzt die vorhandene AVX2-Implementierung, wenn CPU und Betriebssystem sie unterstützen. Die skalare und die SSE2-Prüfung decken die übrigen Fälle ab. Die Prozesszeit sank auf der Messmaschine von 181,6 auf 135,3 ms. [UTF-8-Prüfung](../../include/bolt/detail/Utf8Block.h)
- **String-Schlüssel.** `HashTable.Find` vereinte vor der Änderung 82 % der Stichproben im langen Wörterbuchtest. Der generierte Vergleich behandelt Nullwerte in einem eigenen Zweig und liest bei zwei vorhandenen Strings direkt aus den Headern. Die privaten Metadaten benötigen auf x64 acht Bytes weniger pro String. Die gesamte Arbeitslast lag bei 276,5 → 271,7 ms. [Stringvergleich](../../src/native/lowering/Values.cpp) · [Stringlayout](../../src/native/NativeText.hpp)
- **Bytekopien und Listensuche.** Die CRT-Kopier- und Verschiebeschleifen dominierten die großen Speicherprogramme. Ein Versuch mit AVX-512-Kopien und Füllungen verbesserte `memory-long` bei fester Affinität um 13,1 %, verschlechterte ihn aber in der separat gebauten Kontrolle unter normaler Windows-Planung um 12,3 %. Beides sind Ergebnisse desselben Versuchsaufbaus mit zwei Messserien. Die Daten trennen den Einfluss von Codeanordnung, Adresslayout und Planung nicht. Eine AVX-512-Listensuche verlängerte beide langen Suchprogramme. Beide Versuche wurden aus dem ausgelieferten Build entfernt. [Einzelzeiten der Vorversuche](llvm-release-trials-windows-x64.json)

## Wo der optimierte Build noch Zeit verbringt

- `memory-long` brauchte unter normaler Planung 555,5 ms. Kopieren, Verschieben und Füllen laufen weiter über die Windows-CRT. Die Linkerkarte weist `memcpy` und `memmove` dieselbe Adresse zu. Entsprechend lassen sich ihre Kosten an gemeinsamen Instruktionsadressen nur zusammen angeben.
- `arithmetic` brauchte 402,6 ms. 100 % der Stichproben lagen im übersetzten Programmeinstieg. Die entrollte Schleife führt Rotationen, Shifts und XOR-Operationen auf einem Zustand aus, von dem jeder nächste Schritt abhängt.
- Bei `dictionary-string-long` entfielen weiterhin 82 % der Stichproben auf `HashTable.Find`. Bei `list-edit-long` entfielen 96 % auf die CRT-Routinen, die beim Einfügen und Löschen nachfolgende Elemente verschieben.

## Alle 33 Programme

Die Zeiten sind Mediane vollständiger Prozesse einschließlich Start und Ausgabe. Positive Prozentwerte bedeuten kürzere Laufzeiten. Das Intervall jeder Zeile gilt für das einzelne Programm ohne Korrektur für Mehrfachvergleiche. Die letzten beiden Spalten zeigen den häufigsten Ort der Stichproben unter fester Affinität. Beim Inlining umfasst der Symbolname auch den in die Funktion eingebetteten Code.

| Programm | Windows vorher ms | Windows nachher ms | Ersparnis % [95-%-Bereich] | Fester Kern vorher → nachher ms | Profil vorher | Profil nachher |
|---|---:|---:|---:|---:|---|---|
| `float-parse` | 193,61 | 98,57 | 49,1 [43,7 bis 49,6] | 198,66 → 100,18 | MSVC-Dezimalkonvertierung (32 %) | fast_float (34 %) |
| `utf8-roundtrip-long` | 181,65 | 135,31 | 25,5 [24,3 bis 28,1] | 181,84 → 133,88 | UTF-8-Blockprüfung (50 %) | memcpy_repmovs (58 %) |
| `dictionary-string-long` | 276,51 | 271,67 | 1,8 [1,1 bis 3,5] | 280,37 → 275,51 | HashTable.Find (82 %) | HashTable.Find (82 %) |
| `memory-long` | 560,07 | 555,48 | 0,8 [-0,5 bis 4,5] | 570,40 → 567,71 | memcpy (42 %) | memcpy (39 %) |
| `utf8-roundtrip` | 34,56 | 32,78 | 5,2 [3,0 bis 9,4] | 34,87 → 32,88 | memcpy_repmovs (60 %) | memcpy_repmovs (62 %) |
| `buffer-long` | 119,96 | 119,35 | 0,5 [-4,4 bis 2,7] | 120,93 → 121,82 | ntdll.dll (51 %) | ntdll.dll (52 %) |
| `string-equality` | 29,14 | 28,65 | 1,7 [-0,7 bis 2,7] | 29,73 → 29,32 | übersetzter Bolt-Code (41 %) | übersetzter Bolt-Code (44 %) |
| `dictionary-string` | 15,39 | 15,05 | 2,2 [-0,3 bis 4,6] | 15,51 → 15,52 | HashTable.Find (77 %) | HashTable.Find (73 %) |
| `list-edit` | 21,33 | 21,00 | 1,6 [-2,5 bis 3,9] | 21,15 → 21,11 | memcpy (92 %) | memcpy (86 %) |
| `utf8-index` | 6,43 | 6,22 | 3,3 [-7,0 bis 10,4] | 6,52 → 6,63 | ntdll.dll (96 %) | ntdll.dll (83 %) |
| `struct-copy-small` | 6,60 | 6,41 | 2,9 [-9,7 bis 14,3] | 6,30 → 6,46 | ntdll.dll (89 %) | ntdll.dll (90 %) |
| `memory` | 74,48 | 74,32 | 0,2 [-1,1 bis 0,6] | 75,61 → 75,27 | memcpy (43 %) | memcpy (40 %) |
| `struct-copy-wide` | 8,27 | 8,10 | 2,0 [-4,7 bis 4,8] | 7,88 → 8,16 | übersetzter Bolt-Code (58 %) | übersetzter Bolt-Code (70 %) |
| `arithmetic` | 402,76 | 402,62 | 0,0 [-0,4 bis 1,2] | 404,84 → 406,21 | übersetzter Bolt-Code (100 %) | übersetzter Bolt-Code (100 %) |
| `buffer-record-add` | 8,12 | 8,09 | 0,4 [-3,9 bis 6,6] | 8,31 → 8,22 | übersetzter Bolt-Code (64 %) | übersetzter Bolt-Code (59 %) |
| `buffer-truncate` | 7,97 | 7,96 | 0,2 [-6,7 bis 7,7] | 7,82 → 7,73 | memcpy_repmovs (60 %) | memcpy_repmovs (67 %) |
| `fp-integrate` | 150,10 | 150,09 | 0,0 [-1,9 bis 0,4] | 149,66 → 150,16 | übersetzter Bolt-Code (100 %) | übersetzter Bolt-Code (99 %) |
| `list-fill-clear` | 8,27 | 8,26 | 0,1 [-7,3 bis 8,3] | 8,11 → 8,20 | übersetzter Bolt-Code (69 %) | übersetzter Bolt-Code (54 %) |
| `pointers` | 89,33 | 89,41 | -0,1 [-1,6 bis 0,8] | 90,55 → 90,30 | übersetzter Bolt-Code (99 %) | übersetzter Bolt-Code (98 %) |
| `buffer` | 5,61 | 5,71 | -1,8 [-9,4 bis 10,0] | 5,44 → 5,59 | ntdll.dll (95 %) | ntdll.dll (100 %) |
| `list-search` | 6,34 | 6,48 | -2,3 [-7,5 bis 9,2] | 6,49 → 6,36 | ntdll.dll (97 %) | ntdll.dll (95 %) |
| `string-affixes` | 7,56 | 7,71 | -2,1 [-13,7 bis 7,3] | 7,67 → 7,63 | übersetzter Bolt-Code (57 %) | übersetzter Bolt-Code (52 %) |
| `struct-buffer` | 9,66 | 9,83 | -1,7 [-6,4 bis 6,9] | 9,52 → 9,62 | memcpy_repmovs (75 %) | memcpy_repmovs (78 %) |
| `utf8-index-long` | 189,19 | 189,40 | -0,1 [-1,1 bis 1,1] | 192,40 → 191,87 | übersetzter Bolt-Code (99 %) | übersetzter Bolt-Code (99 %) |
| `queue-worklist-long` | 180,14 | 180,42 | -0,2 [-1,1 bis 1,2] | 184,31 → 183,61 | übersetzter Bolt-Code (99 %) | übersetzter Bolt-Code (100 %) |
| `queue-worklist` | 17,86 | 18,15 | -1,7 [-3,9 bis 0,7] | 17,87 → 18,18 | übersetzter Bolt-Code (83 %) | übersetzter Bolt-Code (92 %) |
| `list-edit-long` | 185,27 | 185,58 | -0,2 [-1,3 bis 2,3] | 188,49 → 188,04 | memcpy (96 %) | memcpy (96 %) |
| `buffer-truncate-long` | 140,35 | 140,67 | -0,2 [-0,9 bis 1,7] | 142,15 → 142,32 | memcpy_repmovs (91 %) | memcpy_repmovs (89 %) |
| `struct-values` | 77,85 | 78,35 | -0,6 [-0,8 bis 0,6] | 78,59 → 78,50 | übersetzter Bolt-Code (98 %) | übersetzter Bolt-Code (98 %) |
| `list-search-u64` | 167,42 | 168,04 | -0,4 [-1,3 bis 0,4] | 171,65 → 171,48 | AVX2-Listensuche (90 %) | AVX2-Listensuche (89 %) |
| `list-search-long` | 84,19 | 85,27 | -1,3 [-2,7 bis 0,1] | 86,71 → 86,32 | AVX2-Listensuche (85 %) | AVX2-Listensuche (83 %) |
| `indirect-array` | 129,07 | 130,25 | -0,9 [-2,8 bis 2,2] | 129,93 → 129,65 | übersetzter Bolt-Code (99 %) | übersetzter Bolt-Code (98 %) |
| `struct-zero` | 128,03 | 131,39 | -2,6 [-12,5 bis 5,5] | 123,55 → 122,94 | übersetzter Bolt-Code (89 %) | übersetzter Bolt-Code (93 %) |

## Messung und Reproduktion

AMD Ryzen 9 7900X, Windows 11 x64, LLVM 23.1.2. Als Ausgangspunkt diente der Release-Compiler aus der vorherigen Primitivenmessung. Der neue Build entstand aus demselben Quellstand mit den vier hier geänderten Laufzeit- und Compilerdateien. Parallele Änderungen am Projekt sind dadurch vom Vergleich ausgeschlossen. Getestet wurde auf diesem Rechner.

Der Treiber kompilierte vorab mit `--release --backend llvm --require-native`. Die Programme liefen pro Runde in zufälliger Reihenfolge, beide Varianten jedes Programms unmittelbar hintereinander in balancierter Reihenfolge. Die erste Serie nutzte logischen Prozessor 8, Seed 1604 und 21 Messrunden. Die zweite Serie verwendete dieselben Programme nach Prüfung ihrer SHA-256-Hashes, reguläre Windows-Planung auf allen 24 logischen Prozessoren, Seed 2941 und elf Messrunden. Vor jeder Serie liefen zwei Aufwärmrunden. Die Rundensumme addiert die 33 Prozesszeiten jeder Variante. Das jeweilige Intervall beruht auf 4.000 Bootstrap-Ziehungen vollständiger Paare.

Die getrennten Profile erfassten den Hauptthread derselben Binärdateien ungefähr jede Millisekunde. Kurze Programme wurden bis zu 128-mal gestartet. Vorher entstanden 18.611 Stichproben, nachher 17.466. Für `buffer` waren es 65 und 33. Die Anteile beziehen sich auf beobachtete Instruktionsadressen und können Wartezeit sowie Lader- und Betriebssystemcode enthalten.

[Einzelzeiten mit regulärer Planung](llvm-release-whole-confirmation-windows-x64.json) · [Einzelzeiten auf festem Kern](llvm-release-whole-windows-x64.json) · [Profil vorher](llvm-release-profile-before-windows-x64.json) · [Profil nachher](llvm-release-profile-after-windows-x64.json) · [Befehle zur Reproduktion](../performance/README.md#llvm-release-über-vollständige-programme)

## Korrektheit

Alle 33 Programme lieferten bei jeder Messung und jedem Profilerstart die feste Sollausgabe. Die Testgruppen `Bolt.Unit` und `Bolt.NativeUnit` bestanden einschließlich der Prüfungen für Zahlenkonvertierungen und UTF-8-Grenzen. 100.000 reproduzierbare Dezimaltexte wurden zusätzlich für beide Zieltypen mit MSVC verglichen. Status, gelesene Länge und bei Erfolg das IEEE-Bitmuster stimmten in allen 200.000 Prüfungen überein.

Die fünf Fixtures `string-equality-widths`, `copied-memory-compare`, `string-index-ownership`, `hash-table-buckets` und `string-snapshot` wurden mit explizitem LLVM Release gegen ihre Sollwerte geprüft und bestanden auch in der bestehenden Paritätssuite. Der Compiler-Host-Test bestand einschließlich Rollback, Entladen und konkurrierendem Neuladen.
