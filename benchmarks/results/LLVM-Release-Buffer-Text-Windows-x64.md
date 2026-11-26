# Bolt LLVM Release auf Windows x64 · Buffer und Text

Am 21. November 2026 sank die Gesamtzeit der 33 Programme auf einem Ryzen 9 7900X bei regulärer Windows-Planung von **2,886 auf 2,713 Sekunden**. Das sind **6,0 % weniger Laufzeit** gegenüber dem unmittelbar vor den Änderungen gebauten Bolt-Compiler. Die C-Vergleichsreihe mit 13 handoptimierten Fassungen brauchte **3,270 Sekunden**. Beide Bolt-Builds enthalten die zuvor gemessenen Queue- und Memory-Optimierungen. [Queue-Vergleich](LLVM-Release-Queue-Windows-x64.md) · [Memory-Vergleich](LLVM-Release-Memory-Windows-x64.md)

| Zeit für alle 33 Programme | Bolt vorher | Bolt nachher | C regulär | C mit spezialisierten Fassungen | Bolt-Ersparnis gegenüber vorher, gepaartes 95-%-Intervall |
|---|---:|---:|---:|---:|---:|
| Reguläre Windows-Planung, 15 Messrunden | 2,886 s | 2,713 s | 5,002 s | 3,270 s | 5,6 bis 6,6 % |
| Logischer Prozessor 8, 11 Messrunden | 3,364 s | 3,191 s | 5,793 s | 3,956 s | −1,7 bis 15,6 % |

Die Gesamtzeit ist der Median der Rundensummen. Die Messungen auf dem festen Kern streuten stärker. Dort beträgt der beobachtete Vorher-nachher-Gewinn 5,1 %, das Intervall schließt Gleichstand ein. Gegenüber den spezialisierten C-Fassungen brauchte Bolt unter regulärer Planung rund 17 % weniger Zeit, auf dem festen Kern rund 19 %.

## Gezielte Änderungen

Die Zeiten in der Tabelle sind Mediane in Millisekunden bei regulärer Windows-Planung. Für den Stringvergleich verwendet die Suite in beiden C-Reihen dasselbe Programm.

| Programm | Bolt vorher | Bolt nachher | C mit Spezialisierungen | Ersparnis gegenüber Bolt vorher, gepaartes 95-%-Intervall |
|---|---:|---:|---:|---:|
| `buffer-long` | 168,19 | 22,92 | 15,35 | 86,0 bis 87,4 % |
| `utf8-roundtrip-long` | 143,71 | 122,02 | 70,68 | 10,9 bis 18,2 % |
| `string-equality` | 30,28 | 24,24 | 25,42 | 15,2 bis 23,0 % |
| `utf8-roundtrip` | 36,51 | 35,51 | 12,23 | −2,2 bis 8,1 % |

- **Buffer.** Wenn die Elemente eines lokalen `Buffer<int>` aus ihrem Index und lokalen Ganzzahlen berechnet werden und die Arraykopie nur gelesen wird, kann Bolt die Werte beim Zugriff berechnen. Der Compiler hält die Eingaben beim Aufbau fest. Eine Addition lässt er nur zu, wenn die möglichen Werte einen Überlauf ausschließen. Der Abstand zur fusionierten C-Schleife sank von 153 auf 7,6 ms. [Compiler](../../src/native/IndexedSnapshot.inc) · [Test für geänderte Eingaben und Überlauf](../../tests/native/indexed-snapshot.bolt)
- **UTF-8.** Beim Ersetzen eines bereits validierten Strings darf Bolt die bekannte Zeichenzahl übernehmen, wenn sich ausschließlich ASCII-Bytes in andere ASCII-Bytes geändert haben. Die Laufzeit prüft diese Bedingung über den gesamten gleich langen Inhalt. Änderungen an Mehrbytezeichen und Strings, deren Bytes an nativen Code herausgegeben wurden, durchlaufen die vollständige Validierung. Der lange Rundlauf bleibt trotz der Einsparung von 21,7 ms der größte Rückstand zur spezialisierten C-Fassung, die zwei vorab reservierte Speicherbereiche wiederverwendet. [Laufzeit](../../src/native/NativeText.hpp) · [UTF-8-Test](../../tests/native/utf8-replacement.bolt) · [C-Fassung](../performance/cases/utf8-roundtrip.c)
- **Stringvergleich.** Bei mehr als 32 Bytes vergleicht der generierte Code zuerst die ersten und letzten acht Bytes. Stimmen die Enden überein, folgt der vollständige Vergleich. Damit sinkt die Bolt-Laufzeit im Einzeltest um 20,0 %. Beim langen String-Dictionary lagen die Mediane bei 266,90 und 267,65 ms, das Vorher-nachher-Intervall schließt Gleichstand ein. [Compiler](../../src/native/lowering/Values.cpp) · [Vergleich an allen Positionen](../../tests/native/string-equality-widths.bolt)

## Messverfahren

Beide Bolt-Compiler wurden aus derselben lokalen Quellkopie mit MSVC gebaut und übersetzten die Benchmarkprogramme mit `--release --backend llvm --require-native`. Clang 23.1.2 übersetzte die C11-Referenzen mit `-O3 -DNDEBUG -march=native -ffp-contract=off`. Für die 20 Programme ohne eigene spezialisierte C-Fassung zählt dieselbe C-Messung in beiden Gesamtsummen. Jede Messung umfasst Prozessstart, Ausführung und Ausgabe. Der Treiber prüft die Ausgabe jedes einzelnen Laufs. Nach zwei Aufwärmrunden variieren Programm- und Variantenreihenfolge. Die Intervalle beruhen auf 4.000 Bootstrap-Ziehungen gepaarter Runden, bei einzelnen Programmen ohne Korrektur für Mehrfachvergleiche. [Rohdaten und Prüfsummen](llvm-release-buffer-text-windows-x64.json) · [Reproduktion](../performance/README.md#buffer-und-text-gegen-die-c-referenzen-messen)

Unter regulärer Planung zeigte kein Einzelintervall eine eindeutige Verschlechterung gegenüber dem Vorher-Build. Auf dem festen Kern stieg `list-fill-clear` zunächst von 9,28 auf 11,29 ms. Ein separater Vergleich auf demselben Kern mit fünf Aufwärm- und 51 Messrunden ergab 6,98 gegenüber 7,10 ms, mit einem 95-%-Intervall von −6,2 bis +10,7 % Laufzeitänderung. Die Disassemblierung der generierten Bolt-Objekte einschließlich Relokationen war identisch. Die Zusatzmessung steht unter `list_fill_clear_confirmation` in den [Rohdaten](llvm-release-buffer-text-windows-x64.json).

Beide Unit-Testgruppen und alle 243 Fälle der LLVM-Release-AOT-Suite bestanden. Sieben gezielte Regressionstests bestanden zusätzlich im LLVM-Release-JIT. Eine separate Prüfung mit dem skalaren UTF-8-Decoder als Referenz verglich 91.104 einzelne Byteänderungen. In allen 8.994 Fällen, in denen die Laufzeit die alte Zeichenzahl übernahm, waren die neuen Bytes gültig und die Anzahl stimmte.
