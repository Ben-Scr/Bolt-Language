# UTF-8-Rundlauf mit bekannter Schreibposition

Bolt LLVM Release AOT benötigt im langen UTF-8-Rundlauf **17,47 ms**. Die vorherige Bolt-Fassung mit Suche nach den geänderten Bytes benötigt **26,23 ms**, die spezialisierte C-Fassung **15,63 ms**. Damit sinkt die Bolt-Laufzeit in der Wiederholungsmessung um 33,4 %. Gegenüber C verbleiben 1,84 ms oder 11,8 %.

| Median der gesamten Prozesslaufzeit | Bolt vorher | Bolt mit Schreibposition | C mit Schreibposition |
|---|---:|---:|---:|
| 48 Starts, Standardplanung | 26,23 ms | 17,47 ms | 15,63 ms |
| 48 Starts, logische CPU 8 | 30,63 ms | 20,50 ms | 18,93 ms |
| Erste Reihe, 32 Starts, Standardplanung | 50,30 ms | 35,02 ms | 37,68 ms |
| Erste Reihe, 32 Starts, logische CPU 8 | 51,64 ms | 30,48 ms | 30,19 ms |

In der ersten Reihe streuten die Prozesszeiten stark. Beide gepaarten 95-%-Bootstrap-Intervalle für das Laufzeitverhältnis Bolt zu spezialisiertem C schlossen 1 ein. In der Wiederholung lagen sie bei 1,080 bis 1,145 unter Standardplanung und bei 1,050 bis 1,142 auf CPU 8. Der Vorteil gegenüber der vorherigen Bolt-Fassung war in allen vier Reihen messbar. [Einzelzeiten und Profile der ersten Reihe](llvm-release-utf8-write-range-windows-x64.json) · [Einzelzeiten und Profile der Wiederholung](llvm-release-utf8-write-range-repeat-windows-x64.json)

## Welche Arbeit entfällt

Für ein lokales `byte[]` aus `text.ToUtf8()` übernimmt der Compiler den beim Schreiben bereits berechneten und geprüften Index. Erfolgt die anschließende Ersetzung desselben Strings in geradlinigem Code, bekommt `FromUtf8` die Grenzen des beschriebenen Bereichs. Ein einzelner Schreibzugriff ergibt genau ein Byte, mehrere Zugriffe ergeben den Bereich vom kleinsten bis zum größten Index. Der erzeugte Code für diesen Benchmark übergibt `[offset, offset + 1)`. Auch bei einem Index mit Seiteneffekt wie `bytes[i++]` wird der Index genau einmal ausgewertet.

Sind altes und neues Byte ASCII, bleibt die Anzahl der Unicode-Skalarwerte gültig. Bei Änderungen an Mehrbytezeichen übernimmt die vollständige UTF-8-Validierung. Gibt das Programm einen Array-Zeiger weiter, ersetzt es den Ausgangsstring oder unterbricht eine Verzweigung die Nachverfolgung, verwendet der Compiler den bisherigen Konvertierungspfad. Ein zuvor herausgegebener nativer String-Zeiger erzwingt weiterhin die erneute Validierung.

Vorher trafen **207 von 530** Hauptthread-Stichproben die Routine zum Suchen und Prüfen der Änderungen, nachher **1 von 521**. Die Kopie zum Erzeugen des unabhängigen Byte-Arrays in `ToUtf8` bleibt mit **320 von 521** Stichproben der größte gemessene Posten. Auch in der spezialisierten C-Fassung entfielen 372 von 516 Stichproben auf die Kopierroutine.

## Messbedingungen und Korrektheit

Gemessen am 21. November 2026 auf einem AMD Ryzen 9 7900X unter Windows 11 x64. Beide Bolt-Builds stammen aus demselben Ausgangsstand. Die unveränderte C-Fassung wurde mit Clang 23.1.2, `-O3 -DNDEBUG -march=native -ffp-contract=off` und `BENCH_UTF8_KNOWN_EDIT` gebaut. Der bestehende Messlauf führt bei jeder Variante 12.288 Runden auf 49.152 UTF-8-Bytes aus. Nach vier Aufwärmstarts wurden die Programme in balancierter, zufälliger Reihenfolge ausgeführt. Die Stichproben wurden separat erhoben. Sämtliche Starts lieferten die erwartete Prüfsumme `409522176`. [Messbefehl](../performance/README.md#langer-utf-8-rundlauf)

Alle 246 Fälle im LLVM-Release-AOT-Testkorpus bestanden. Die beiden gezielten UTF-8- und Besitztests bestanden zusätzlich im Fast-JIT, Fast-AOT und LLVM-Release-JIT. Unter anderem geprüft wurden mehrere Schreibstellen, Index-Inkrement, lebende Kopien, unberührte Arrays, Bereichsfehler und ungültiges UTF-8. Ein zusätzlicher Vergleich der Laufzeitroutine mit einem skalaren Decoder ergab bei 7.303 gültigen und 1.097 ungültigen Ersetzungen dieselben Ergebnisse.
