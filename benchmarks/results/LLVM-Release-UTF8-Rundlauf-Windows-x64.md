# Langer UTF-8-Rundlauf auf Windows x64

Der optimierte Bolt-LLVM-AOT-Build braucht **26,24 ms** für den langen Rundlauf. Der vorherige Build brauchte **124,20 ms**, die bestehende SIMD-C-Referenz **69,42 ms**. Eine zusätzliche C-Fassung, deren Aufrufer die Position des einzigen Schreibzugriffs kennt, erreicht **15,47 ms**.

| Median der gesamten Prozesslaufzeit | Windows-Standardplanung | An logische CPU 8 gebunden |
|---|---:|---:|
| Bolt vor der Änderung | 124,20 ms | 123,49 ms |
| Bolt nach der Änderung | 26,24 ms | 26,17 ms |
| C mit vollständiger SIMD-Validierung | 69,42 ms | 69,77 ms |
| C mit bekannter Änderungsposition | 15,47 ms | 15,48 ms |

Bei Standardplanung sank die Bolt-Laufzeit um 78,9 %. Das Verhältnis der Mediane von Bolt nachher zu vorher betrug 0,211 mit einem gepaarten 95-%-Bootstrap-Intervall von 0,205 bis 0,219. Gegen die bestehende SIMD-C-Fassung betrug das Verhältnis 0,378 (0,365 bis 0,389), gegen die auf die Schreibposition spezialisierte C-Fassung 1,697 (1,635 bis 1,768).

## Wo die Zeit blieb

Vor der Änderung entfielen 922 von 1.461 Hauptthread-Stichproben auf Kopierroutinen. Die Aufrufstapel ordnen 458 davon `ToUtf8` und 464 dem Wiederaufbau des Strings in `FromUtf8` zu. In beiden Richtungen wurde pro Runde der gesamte 48-KiB-Inhalt kopiert. Hinzu kamen wiederholte Speicheranforderungen für die Byte-Arrays.

Bei der Ersetzung eines mindestens 4 KiB großen Heap-Strings vergleicht Bolt den vorhandenen Text und das neue Byte-Array in 64-Byte-Blöcken. Haben sich ausschließlich ASCII-Bytes geändert, bleibt die bereits geprüfte Unicode-Skalarzahl gültig. Nach der ersten solchen Ersetzung kann die Laufzeit den vorhandenen Textspeicher an einen neuen String-Header übergeben und nur den Bereich zwischen den abweichenden Blöcken kopieren. Für `ToUtf8` darf die Laufzeit einen freigegebenen Snapshot-Speicher der gleichen Größe erneut befüllen. Noch lebende Strings und Byte-Arrays behalten ihre eigenen Daten. Bei geänderten Nicht-ASCII-Bytes oder zuvor herausgegebenen nativen String-Zeigern validiert Bolt das gesamte Ergebnis.

Nach der Änderung entfielen 3 von 525 Stichproben auf eine Kopierroutine unter `FromUtf8`. Der größte verbleibende Posten war der Vergleich auf ASCII-Änderungen mit 239 von 525 Stichproben. Weitere 172 entfielen auf die Kopie für `ToUtf8`. In der bestehenden C-Fassung trafen 464 von 671 Stichproben den SIMD-Validator und 180 eine Kopierroutine. Die Stichproben stammen aus separaten Läufen mit Stack-Unwinding.

## Was die C-Vergleiche messen

Alle vier Programme verarbeiten 49.152 UTF-8-Bytes pro Runde und führen 12.288 Runden aus. Der Schleifenkörper schreibt jeweils ein ASCII-Byte in den Snapshot. In den ersten 4.096 Runden ändert sich der Wert an jeder besuchten Position. In den folgenden 8.192 Runden wird dort derselbe Wert erneut geschrieben. Jedes Programm summiert die Skalarzahlen, die geschriebenen Bytes und am Ende den gesamten Byte-Inhalt. Jeder Start ergab `409522176`.

Die bestehende C-Fassung hält zwei reservierte Puffer und validiert jeden neuen Snapshot vollständig mit SIMD. Die zusätzliche C-Fassung nutzt dieselbe Schleife. Ihr Aufrufer kennt die einzige beschriebene Position und kann dort unmittelbar prüfen, ob der Schreibzugriff die UTF-8-Gültigkeit oder Skalarzahl beeinflussen kann. Die allgemeine Bolt-Funktion bekommt das komplette Byte-Array übergeben und muss die Änderungen selbst finden.

## Messung und Korrektheit

Gemessen am 21. November 2026 auf einem AMD Ryzen 9 7900X unter Windows 11 x64. Beide Bolt-SDKs verwendeten dieselbe Compilerdatei, dieselbe Bolt-Quelle und aus demselben Ausgangsstand gebaute Release-AOT-Laufzeiten. Beide C-Fassungen wurden mit Clang 23.1.2, C11, `-O3 -DNDEBUG -march=native -ffp-contract=off` gebaut. Pro Variante und CPU-Einstellung gab es vier Aufwärmstarts und 32 gemessene Starts in balancierter, zufälliger Reihenfolge. Die Zeit enthält den Prozessstart. Die Prüfsumme wurde bei jedem Start kontrolliert.

Eine frühere Messreihe bei höherer Hintergrundlast ergab unter Standardplanung 192,39 ms für Bolt vorher, 42,06 ms für Bolt nachher, 108,27 ms für SIMD-C und 29,67 ms für C mit bekannter Schreibposition. Die Tabelle zeigt die Wiederholung mit neuem Zufallsstartwert. Die JSON-Datei enthält die Einzelzeiten beider Reihen, die Build-Fingerabdrücke und die Stichproben der Wiederholung. [Rohdaten](llvm-release-utf8-roundtrip-windows-x64.json) · [Messbefehl](../performance/README.md#langer-utf-8-rundlauf)

Alle 244 Fälle des direkten LLVM-Release-AOT-Testkorpus bestanden. Die erweiterten Regressionstests prüfen unter anderem 4.095, 4.096 und 4.097 Bytes, unabhängige alte Kopien, ungültige Eingaben, native Schreibzugriffe und die Erkennung einer Ersetzung während einer Iteration. Ein separater Differenztest verglich 8.400 Ersetzungen mit einem skalaren UTF-8-Decoder. Alle 7.303 gültigen und 1.097 ungültigen Eingaben ergaben das erwartete Ergebnis.
