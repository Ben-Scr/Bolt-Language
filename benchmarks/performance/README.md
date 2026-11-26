# Bolt-Benchmarks

## Einzelne primitive Operationen

[`primitives.py`](primitives.py) erzeugt für jede Operation einen eigenen Bolt-Kernel. Die Matrix deckt alle sieben Integer-Typen, `float` und `double` ab. Sie enthält Arithmetik, Vergleiche, Bitoperationen, Casts, logische Ausdrücke, Zählschleifen, Bereichsschleifen, Präfix- und Postfixoperationen, Kopien sowie Array- und Pointerzugriffe. Die Ergebnistabelle für Windows x64 enthält jeden gemessenen Fall. [Messwerte und Randbedingungen](../results/Primitive-operations-windows-x64.md)

```sh
python primitives.py --before /path/to/before/bolt --bolt /path/to/after/bolt --passes 512 --warmup 2 --runs 15 --json native.json
python primitives.py --before /path/to/before/bolt --bolt /path/to/after/bolt --backends fast-jit llvm-jit --passes 256 --warmup 2 --runs 9 --json jit.json
python primitives.py --before /path/to/before/bolt --bolt /path/to/after/bolt --backends interpreter --passes 4 --warmup 1 --runs 7 --json interpreter.json
python primitives.py --bolt /path/to/after/bolt --backends cpp --passes 512 --warmup 2 --runs 11 --json cpp.json
python primitives.py --before /path/to/before/bolt --bolt /path/to/after/bolt --cases int.add int.eq loop.foreach --json selected.json
```

Die Programme erhalten Seed und Arbeitsmenge als Laufzeitargumente. Jeder Pass verarbeitet 1.024 Elemente. Der Timer läuft innerhalb des Programms um die jeweilige Schleife. Initialisierung, Ausgabe und Prozessstart liegen außerhalb des Messintervalls. Beim Struct-Kopieren verändert jede Iteration ein Zielfeld. Nach der Zeitmessung gehen alle Felder aller Ziele in die Prüfsumme ein. Der Treiber kontrolliert jede Ausgabe mit einer separaten Python-Rechnung und speichert die Einzelmessungen in 100-ns-Ticks. Die Zeit pro Iteration umfasst auch Eingabezugriffe, Schleifensteuerung und Ergebnisreduktion. Bei kleinen Integer-Typen gelten Bolts normale Promotionsregeln.

Standardmäßig laufen Fast Debug und LLVM Release als AOT-Programme. Die JIT-Profile messen die Ausführung im Speicher nach der Kompilierung. Der C++-Pfad benötigt einen konfigurierten C++-Compiler. Der Interpreter verwendet indizierte Array-Eingaben und lässt die beiden expliziten Pointer-Fälle aus. Für Vergleiche zwischen zwei SDKs müssen Profil, Seed und Arbeitsmenge übereinstimmen.

## Laufzeit ganzer Programme

Die Suite baut jedes Programm in einem neuen temporären Verzeichnis, prüft seine Ausgabe gegen feste Sollwerte und misst die Laufzeit als eigener Prozess. Sie benötigt Python 3.9+, einen C-Compiler und für die Bolt-Profile eine zum Compiler passende Runtime sowie den nativen Linker. Ergebnisse und Fehler landen in einer JSON-Datei. Ein fehlgeschlagener Build, ein Timeout oder ein falsches Ergebnis ergibt einen Fehlerstatus.

## Vollständiger Release-Vergleich

Mit `--regular` läuft jede reguläre Arbeitslast einmal je Compiler. `--before` nimmt ein zweites Bolt-SDK in dieselben Messrunden auf. `--build-runs` misst vollständige Übersetzung und Linken in jeweils einem frischen temporären Verzeichnis. Ab zwei Build-Messungen geht eine zusätzliche Aufwärmrunde voraus. Die Reihenfolge der Compiler wechselt je Runde mit dem angegebenen Zufallsseed. Das Betriebssystem kann Dateien im Speicher behalten. Jede erzeugte Anwendung muss die feste Sollausgabe liefern.

```sh
python suite.py --regular --before /opt/before/bin/bolt --bolt /opt/after/bin/bolt --cc clang --cflag=-ffp-contract=off --build-runs 7 --warmup 2 --runs 15 --json comparison.json
```

Die JSON-Daten enthalten die Compileraufrufe, die Reihenfolgen und für jedes Programm zwei getrennte Messreihen. `profiles.<Profil>.compile_link` enthält Buildzeiten vom Start der Compiler-CLI bis zu ihrem Ende. `profiles.<Profil>.median_ms` und `raw_ms` beziehen sich auf die Laufzeit der fertigen Anwendung einschließlich Prozessstart und Ausgabe. `before-release` bezeichnet das frühere Bolt-SDK.

Der [Windows-x64-Vergleich über 37 Arbeitslasten](../results/Compiler-Runtime-C-Windows-x64.md) enthält Übersetzungs- und Laufzeiten für beide Bolt-Builds und Clang. Die zusätzliche Frontend-Messung verwendet `bolt check` und erzeugt für den zweiten Fall eine generische Klasse mit 4.096 Update-Anweisungen.

```sh
python compiler_check.py --before /opt/before/bin/bolt --bolt /opt/after/bin/bolt --warmup 1 --runs 15 --json compiler-check.json
```

`text-encoding` erzeugt 18 Binärblöcke mit wechselndem Inhalt und 2.046 bis 2.048 Bytes. Jede Runde schreibt Hex und Base64, dekodiert Base64 und prüft alle Bytes der drei Ergebnisse mit einer gemeinsamen 32-Bit-FNV-1a-Summe. Bolt liest die beiden Ergebnisstrings über `ToUtf8()`. Die Längen decken alle drei Base64-Abschlussfälle ab, bei Hex wechseln Groß- und Kleinschreibung. Eine unabhängige Rechnung mit Pythons `bytes.hex`, `base64.b64encode` und `base64.b64decode` ergibt `1825623425`. Die C-Fassung verwendet Heap-Puffer und prüft Zeichen und Padding.

`string-concat` setzt 500.000 Pfadnamen aus ASCII-Teilen zusammen und liest jeweils ein datenabhängiges Byte. Die C-Fassung schreibt die Teile in einen lokalen Puffer fester Größe.

`--all` führt alle Laufzeitfälle einschließlich der stärker optimierten C-Referenzen aus. Für jede Kombination baut die Suite C11 mit `-O3 -DNDEBUG` beziehungsweise MSVC `/O2` und Bolt mit `--release --backend llvm --require-native`. Vor und während der Messungen müssen Exitcode und Ausgabe der Programme zum festen Sollwert passen. Die Reihenfolge innerhalb jedes Paars wird zufällig bestimmt.

```sh
python suite.py --all --bolt bolt --cc gcc --profiles c release --warmup 4 --runs 35 --seed 1601 --json gcc-release.json
python suite.py --all --bolt bolt --cc clang --profiles c release --warmup 4 --runs 35 --seed 1603 --json clang-release.json
python suite.py --cases list-search-long-avx2 list-search-u64 --bolt bolt --cc clang --cflag=-mavx2 --json search-avx2.json
python suite.py --cases struct-copy-small struct-copy-wide string-affixes list-fill-clear buffer-record-add --bolt bolt --cc clang --profiles c release --json primitives.json
```

Die zusätzlichen Arbeitslasten verwenden unter anderem `Buffer.Truncate`, `AppendRange`, Struct-Arrays, `Queue`, `List`, `Dictionary<string,int>`, Stringvergleich und Unicode-Skalarzugriffe. Für UTF-8-Rundläufe kann C denselben SIMD-Validator verwenden. Weitere C-Fassungen speichern Hashwerte vorab, durchsuchen vier SIMD-Vektoren in einem Schleifendurchlauf oder verwenden einen Ringpuffer. Fälle mit `-long` führen dieselbe Berechnung häufiger aus. Die Rohdaten enthalten die einzelnen Prozesszeiten, Toolchain- und Quellfingerabdrücke sowie ab 15 Wiederholungen das gepaarte Bootstrap-Intervall.

Beide Mediane müssen mindestens 200 ms erreichen, damit die Standardqualifikation für eine Laufzeitaussage erfüllt ist. Kurze Programme zeigen zusätzlich den Einfluss von Prozessstart und Systemlast. [Ergebnisse und Messbedingungen für Bolt 0.16.0](results/Release6.md)

Die beiden Struct-Kopiertests weisen Array-Elemente mit 8 bzw. 96 Bytes einander zu, verändern anschließend das Ziel und prüfen die Summe aller Felder. Der Stringtest vergleicht Präfixe und Suffixe einschließlich leerer und zu kurzer Eingaben. Die Collection-Tests befüllen und leeren eine reservierte Liste bzw. einen Struct-Buffer und lesen alle Werte zurück. Die C-Vergleichsprogramme für diese beiden Fälle verwenden Arrays fester Kapazität.

`--cases list-materialized` füllt eine reservierte Liste mit 2.097.151 Ganzzahlen, kopiert sie mit `ToArray()` und ändert in der Liste jedes 193. Element. Zwei Prüfsummen kontrollieren den Inhalt der Kopie und der geänderten Liste. Die C-Fassung verwendet zwei Heap-Arrays und `memcpy`.

## Längere Arbeitslasten für kurze Programme

`--sustained` verwendet neun Vergleichspaare mit mehr Arbeit pro Prozess. Die Queue verarbeitet 240 Millionen Schritte. Die Multiplikation des Schrittzählers erfolgt für diese Fassung in beiden Sprachen als vorzeichenlose 32-Bit-Rechnung. Damit bleibt das Ergebnis über den gesamten Eingabebereich definiert. Die neun festen Sollwerte stammen aus separat berechneten Referenzergebnissen und wurden zusätzlich mit GCCs UndefinedBehaviorSanitizer geprüft. Die Sollwerte lassen sich mit `python3 reference_sustained.py` unabhängig von beiden Compilern nachrechnen. Falls NumPy installiert ist, berechnet das Skript die 240 Millionen Queue-Operationen blockweise.

| Vergleich | Arbeitsmenge pro Prozess |
|---|---:|
| Struct-Kopie mit 96 Bytes | 262.147 Durchgänge über 1.024 Datensätze |
| Queue mit zwei C-Stacks sowie zusätzliche C-Ringpuffer-Kontrolle | 240.000.000 Schritte je Variante |
| Listensuche gegen vier entrollte SIMD-Vektoren in C | 1.048.576 Suchvorgänge |
| UTF-8-Rundlauf mit identischem SIMD-Validator in C | 49.152 Rundläufe |
| Dezimalparser | 3.000.031 Paare aus Float- und Double-Konvertierung |
| Stringgleichheit | 48.000.000 Vergleiche |
| Struct-Buffer mit Kopie und Mutation | 65.536 Frames |
| Frisch nullinitialisiertes Struct-Array | 4.096 Frames |

```sh
python3 suite.py --sustained --bolt bolt --cc clang-19 --profiles c release --warmup 4 --runs 29 --seed 1611 --json sustained-clang19.json
python3 suite.py --sustained --bolt bolt --cc gcc --profiles c release --warmup 4 --runs 29 --seed 1612 --json sustained-gcc.json
```

## Release und Debug messen

Im folgenden Beispiel ist das Arbeitsverzeichnis der Ordner mit dieser README. Unter Windows funktionieren dieselben Python-Befehle in einer Developer PowerShell mit `--cc cl`. Für Linux und macOS kann `--cc clang` oder `--cc gcc` gewählt werden.

```sh
python3 suite.py --cc gcc --bolt bolt --profiles c release --json results/release.json
python3 suite.py --profiles c debug release --cases pointers --json results/pointers.json
python3 suite.py --profiles c --json results/c-only.json
```

- `c` verwendet GCC/Clang mit `-O3 -DNDEBUG` beziehungsweise MSVC mit `/O2 /DNDEBUG`. Die Standardeinstellungen fügen keine CPU-Erweiterungen, LTO, PGO oder Fast-Math hinzu. Zusätzliche Optionen können mit `--cflag=...` beziehungsweise `--boltflag=...` angegeben werden.
- `release` fordert direktes LLVM-AOT an. `debug` verwendet die automatische native Backendwahl des Hosts. Beide verwenden `--require-native`. Die Compiler-Ausgabe im Ergebnis enthält das gewählte Backend. `cpp-release` wählt den C++-Kompatibilitätspfad ausdrücklich. Dessen Zahlen gehören zu einer eigenen Serie.
- Jede Runde mischt die Reihenfolge der Profile mit einem gespeicherten Seed. Standard sind 2 Aufwärmrunden und 15 gemessene Runden pro Profil. Das Ergebnis enthält CPU, Betriebssystem, Compiler, Befehle, Quell- und Binärhashes, verstrichene Buildzeit, Rohzeiten, Median und p95 nach dem Nearest-Rank-Verfahren.
- `c_over_release.speedup` ist der Quotient der beiden Mediane. Werte über 1 stehen für kürzere Bolt-Laufzeit. Die Vorgabe für einen verwertbaren Quotienten beträgt mindestens 15 Messrunden nach dem Aufwärmen und mindestens 200 ms Median für beide Programme. Kurze Fälle sind weiter als Korrektheits- und Strukturtests nutzbar.
- Für einen schnellen CI-Test reichen `--warmup 0 --runs 1` und die Sollausgaben. Gegenüberstellungen absoluter Laufzeiten brauchen feste CPU-/ISA- und Energieeinstellungen, vergleichbare Last und getrennte Hardwareberichte.

## Was die vier Programme tatsächlich messen

| Fall | Gleiche Arbeit in C und Bolt | Sollausgabe |
|---|---|---|
| `arithmetic` | 500 Millionen abhängige Schritte auf einem 64-Bit-Wert mit XOR, Rotation und Rechtsverschiebung. Der Index bleibt positiv und innerhalb von `int32`. Alle Schiebeweiten sind gültig. | `17925543158443646234` |
| `pointers` | 64 Millionen Schritte auf zwei lokalen 32-Bit-Werten. Zwei gültige Pointer werden datenabhängig getauscht. Beide Endwerte gehen in die Ausgabe ein. | `3495758670`, dann `313794096` |
| `memory` | 250.000 Runden mit 1 bis 16.384 Bytes. Zwei getrennte, versetzte Bytebereiche werden gesetzt und kopiert. Das letzte Byte wird verändert, die Vergleichsrichtung ausgewertet und ein überlappender Bereich nach rechts verschoben. C prüft Null, Adressüberlauf und Copy-Overlap und normiert das Vorzeichen von `memcmp`. | `63747311` |
| `buffer` | 524.288 triviale 32-Bit-Werte einzeln anhängen, eine unabhängige Arraykopie bilden und über deren Elemente summieren. C nutzt einen dynamisch wachsenden Puffer. Die interne Kapazität ist Implementierungsdetail. | `17179607040` |

Jeder Eingabebereich ist festgelegt. Die Tabelle deckt gültige Inputs ab. Fehlerfälle für Überschneidungen, Nullpointer und Lebensdauer gehören in die separaten Laufzeit- und Compilertests. Die Speicherzeit umfasst Set, Copy, Compare und Move zusammen. Der Buffertest erfasst Wachstum, Arraykopie und Iteration gemeinsam. Compiler dürfen Arbeit nach den üblichen Optimierungsregeln eliminieren, sofern das Ergebnis erhalten bleibt.

## Längerer Container-Test

Der optionale Fall `buffer-long` wiederholt den Pufferaufbau 256-mal. Jede Runde startet mit neuem Container, hängt 524.288 Werte an, erzeugt eine unabhängige Arraykopie, summiert die Elemente und gibt beide Speicherbereiche frei. Jeder Wert enthält zusätzlich `batch & 31`. Die fest berechnete Endsumme beträgt `4400059777024`. Die vier Standardfälle bleiben die Vorgabe für `suite.py`.

```sh
python3 suite.py --cases buffer-long --cc gcc --bolt bolt --profiles c release --warmup 1 --runs 15 --json results/buffer-long.json
```

## Längerer Test der Byteoperationen

`memory-long` führt dieselbe Set-/Copy-/Compare-/Move-Folge wie `memory` zwei Millionen Mal aus. Der Runner übernimmt dieselben Quelldateien und setzt im temporären Buildverzeichnis ausschließlich die Schleifengrenze von 250.000 auf 2.000.000. Der JSON-Bericht enthält die Hashes der Vorlage und der übersetzten Datei. Beide Versionen verarbeiten dieselben Blockgrößen, Ausrichtungen und Überlappungsrichtungen.

```sh
python3 suite.py --cases memory-long --cc gcc --bolt bolt --profiles c release --warmup 1 --runs 15 --json results/memory-long.json
```

Die Prüfsumme lässt sich unabhängig von den Speicherfunktionen berechnen. Das letzte Byte enthält nach der gezielten Änderung `value ^ 128`. Der anschließende überlappende Move verschiebt genau dieses Byte an die geprüfte Position. Ein skalares Python-Referenzprogramm liefert für die lange Folge `509997311` und für den kurzen Fall `63747311`.

```python
total = 0
for step in range(2_000_000):
    value = (step ^ total) & 255
    last = value ^ 128
    sign = (value > last) - (value < last)
    total += last + sign + 1
print(total)
```

## Analyse und Regressionswarnungen

```sh
python3 suite.py --profiles c release --cases pointers --runs 1 --warmup 0 --codegen-dir codegen --json results/ir-check.json
python3 regression.py results/before.json results/after.json --profile release --threshold-percent 10 --json results/delta.json
```

`--codegen-dir` speichert die disassemblierte fertige Datei über `objdump` oder `--objdump llvm-objdump`. Für die direkten Bolt-Profile kommt die von `emit-ir` gelieferte IR vor LLVM-Optimierung hinzu. Pro Fall enthält der Bericht auch die einmalige Buildzeit. Laufzeiten einzelner Compilerphasen, Cache-Misses und Allokationszähler erfordern zusätzliche Instrumentierung.

`regression.py` prüft Hostdaten, Quellhash, Sollwert und Compilerargumente. Wenn beide Berichte die CPU-Affinität enthalten, muss auch die Menge der zugelassenen CPUs übereinstimmen. Der Bericht kennzeichnet Fälle mit mehr als 10 % längerer Medianlaufzeit als Warnung. Bei zu kurzen oder zu wenigen Messungen wird der Vergleich ausgelassen. Für einen kontrollierten Runner bewirkt `--fail-on-warning` einen Fehlerstatus bei Warnung oder unzureichender Messdauer. Für eine Zielvorgabe relativ zu C gibt es im Suite-Runner separat `--min-speedup`.

## Debug-Iteration gegen C#

```sh
python3 edit_latency.py --bolt bolt --dotnet dotnet --json results/edits.json
python3 edit_latency.py --languages bolt --json results/edits-bolt.json
python3 edit_latency.py --languages bolt --bolt /opt/new/bin/bolt --baseline-bolt /opt/old/bin/bolt --json results/edits-before-after.json
```

Pro Runde ändert sich der Rückgabewert derselben Methode in einem Ein-Datei-Projekt. Die Messung beginnt, sobald die neue Quelldatei geschlossen ist. Sie endet mit dem Prozessende nach Ausgabe des neuen Werts. Der Bericht schlüsselt Build, Programmstart samt Ausführung und Gesamtdauer auf. Jede Fassung hat einen einmaligen Sollwert, sodass ein veralteter Cachetreffer die Prüfung nicht bestehen kann.

Bolt nutzt die normale Debug-CLI und den nativen Linker. C# nutzt das lokal installierte .NET-SDK, einen Debug-Build mit `--no-restore` und einen neuen Laufzeitprozess. Der einmalige Restore für das paketfreie Projekt liegt außerhalb der Messung. Der erste Build wird separat erfasst. Warmmessungen dürfen die jeweils normalen Compiler-Caches und Compiler-Server wiederverwenden. Das misst den CLI-Arbeitsablauf für eine kleine Änderung. Ein laufender Funke-Editor, das Einspielen eines Methodendeltas in einen aktiven Prozess und die Zeit bis zum nächsten Frame sind eigene Messungen.

Mit `--baseline-bolt` nimmt eine frühere Bolt-Version in einem eigenen Arbeitsverzeichnis an denselben Änderungen teil. Die Reihenfolge der aktiven Compiler wird für jede Änderung mit dem protokollierten Seed gemischt.

## Zwei bereits gebaute Programme vergleichen

Der bestehende Einzelvergleich bleibt verfügbar.

```sh
python3 compare.py --baseline ./arithmetic-c --candidate ./arithmetic-bolt --name arithmetic --runs 21 --json results/pair.json
```

Hier erfolgt die Korrektheitsprüfung durch gegenseitigen Ausgabevergleich. In `suite.py` prüft jeder Lauf die Ausgabe zusätzlich gegen den festen Sollwert des ausgewählten Falls.

## Zwei Bolt-SDKs mit C im Buffer-Test vergleichen

Der Linux-Vergleich verwendet den unveränderten langen Buffer-Test für die Gesamtlaufzeit. Ein zweites Programmpaar misst die gleichen 256 Chargen mit je 524.288 Elementen. Es erfasst pro Charge fünf Zeitpunkte zum Trennen von Anhängen, `ToArray`, Lesen und Freigabe. Beide Bolt-SDKs laufen gegen die jeweils eigene Runtime. Jeder Lauf muss die unabhängige Prüfsumme 4.400.059.777.024 ergeben.

```sh
python3 buffer_comparison.py --before /pfad/zum/alten/sdk/bin/bolt \
  --after /pfad/zum/neuen/sdk/bin/bolt --cpu 2 --runs 15 --json results/buffer-comparison.json
```

Für `--cpu` eine im Prozess verfügbare logische CPU einsetzen. Der Runner bindet alle drei Programme an diese CPU, verwirft einen Warmlauf je Programm und variiert die Reihenfolge innerhalb jeder Dreiergruppe. Bei 15 Runden hat jedes Programm jede Position fünfmal. Die JSON-Datei enthält Einzelzeiten, Reihenfolge, Phasenmediane, Prüfsummen der Werkzeuge und die tatsächlich aufgelösten Runtime-Versionen.

Der aktuelle Release-Vergleich enthält auch eine von Hand in Kapazitätsabschnitte geteilte C-Schleife. Dafür beim gleichen Runner `--chunked-c` angeben. [Messwerte und Reproduktion](results/Buffer-Release3.md)

## Zusätzliche Release-Vergleiche

| Fall | Arbeit pro Prozess | Kontrolle |
|---|---|---|
| `struct-values` | 96 Millionen Updates eines Structs aus zwei `uint`, mit lokaler Kopie, Übergabe als Wert, Rückgabewert und Zugriff auf den früheren Zustand | Beide ausgegebenen 32-Bit-Werte müssen übereinstimmen. |
| `fp-integrate` | 128 Millionen voneinander abhängige Double-Updates mit wechselndem Impuls | Vergleich des vollständigen 64-Bit-Musters. Beide Programme verwenden getrennte Multiplikation und Addition auf dem generischen x86-64-Ziel. |
| `indirect-array` | 48 Millionen abhängige Indizierungen in einem Array mit 65.536 Einträgen | Die positive Summe aller gelesenen Werte muss stimmen. Die C-Referenz prüft Index und Vorzeichen. |

Alle neun Fälle einschließlich der kurzen Prozesse lassen sich gemeinsam prüfen. Die kurze Variante von `memory` oder `buffer` erreicht auf üblichen Rechnern die sonst eingestellten 200 ms Mindestlaufzeit selten. Für den Vergleich der tatsächlichen Prozesszeiten wird sie hier ausdrücklich einbezogen.

```sh
python3 suite.py --bolt /pfad/bin/bolt --cc gcc --profiles c release \
  --cases arithmetic pointers memory memory-long buffer buffer-long struct-values fp-integrate indirect-array \
  --warmup 4 --runs 40 --min-duration-ms 0 --min-speedup 1 --min-p95-speedup 1 --json release.json
```

Die beiden Schwellen prüfen Median und p95 getrennt. Beide müssen pro Fall mindestens Gleichstand mit C erreichen. Die JSON-Datei enthält auch nach einem verfehlten Ziel alle Ergebnisse, die Reihenfolge, Compiler- und Programmprüfsummen, Konsolenausgaben als Sollwerte und die gepaarten Einzelzeiten. Das 95-%-Bootstrapintervall beschreibt die Unsicherheit des Medianverhältnisses innerhalb dieser Messserie. Ein Intervall, das 1 enthält, lässt die Reihenfolge offen.

## Bytekopie und Vergleich

Der Langlauf vergleicht Bolt sowohl mit dem üblichen C-Aufruf von `memcmp` als auch mit einer C-Referenz, die nur das möglicherweise veränderte Byte prüft. [Messwerte und Reproduktion](results/Memory-Release4.md)

Der aktuelle Kurztest erfasst zusätzlich zwei C-Fassungen mit Eigentumsübertragung und eine C-Fassung mit expliziter Schleifenfusion. Mit `--gate` müssen sowohl Median als auch p95 gegenüber jeder Referenz bestehen. Bei Messungen dieses Falls ist zu berücksichtigen, dass beide Compiler einen ausschließlich lokal beobachtbaren Speicherzugriff vereinfachen dürfen. [Kurzbuffer und C-Kontrollen](results/Buffer-Release4.md)

## Indirekte Array-Zugriffe

Der Indexlauf misst 48 Millionen voneinander abhängige Zugriffe auf eine lokale Tabelle. Die zusätzliche C-Referenz drückt den nachgewiesenen Wertebereich der Tabelle von Hand aus. [Messwerte und Reproduktion](results/Index-Release4.md)

Für die sechs zusätzlichen C-Gegenproben heißen die Fälle `pointers-mask`, `buffer-fused`, `buffer-long-fused`, `indirect-array-unchecked`, `memory-long-prefix` und `buffer-long-fused-unroll8`. Der Treiber übersetzt jeweils dasselbe Bolt-Programm wie beim ursprünglichen Vergleich. C kann bei diesen Varianten den lokalen Pointertausch ohne Sprung ausdrücken, die Buffer-Werte direkt summieren, bekannte Indexgrenzen voraussetzen oder einen nachweislich identischen Bytebereich überspringen. Alle ursprünglichen C-Varianten bleiben separat ausführbar.

`float-parse` vergleicht eine Million Single-/Double-Konvertierungspaare aus acht fest vorgegebenen Eingabetexten. Die Referenz verwendet `strtof` und `strtod`. Alle Eingaben ergeben endliche Werte. Die C-Fassung prüft das Ende jedes Eingabetextes. Der gemeinsame 64-Bit-Prüfwert hängt von den exakten Bitmustern beider Gleitkomma-Ergebnisse ab.

## Release-Code zweier Bolt-SDKs vergleichen

### LLVM Release über vollständige Programme

Der Vergleich der 33 ursprünglichen Programme misst jede Variante vom Prozessstart bis zum Ende der Ausgabe. Eine Runde enthält jedes Programm genau einmal je SDK. Die Reihenfolge der Programme wird gemischt, die Reihenfolge der beiden Varianten innerhalb jedes Programmpaars ist balanciert. Der Modus speichert die einzelnen Zeiten, die Summe jeder Runde sowie gepaarte 95-%-Bootstrapintervalle. [Messwerte und Engpässe auf Windows x64](../results/LLVM-Release-Gesamtlaufzeit-Windows-x64.md)

Die folgenden Befehle laufen in PowerShell 7 aus dem Projektstamm in einer x64-Entwicklerumgebung von Visual Studio. Für `--keep-builds` wird ein neues Verzeichnis verwendet. Die Linker-Symbolkarten erlauben, dieselben Binärdateien anschließend zu profilieren.

```powershell
$cases = (Get-Content benchmarks/results/primitive-workloads-windows-x64.json -Raw | ConvertFrom-Json -AsHashtable)['cases'].Keys
$dir = Join-Path $env:TEMP ("bolt-release-" + [guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $dir
$before = 'build/primitives/bin/bolt.exe'
$after = 'build/llvm-release/bin/bolt.exe'
$env:LINK = '/MAP'
(Get-Process -Id $PID).ProcessorAffinity = 256
python benchmarks/performance/release_comparison.py --before $before --after $after --cases $cases --suite-rounds --keep-builds "$dir/programs" --warmup 2 --runs 21 --json "$dir/times.json"
cl /nologo /std:c++20 /O2 /EHsc benchmarks/performance/windows_sampler.cpp "/Fe:$dir/sampler.exe" "/Fo:$dir/sampler.obj" winmm.lib
python benchmarks/performance/profile_windows.py --bolt $before --programs "$dir/programs/before" --sampler "$dir/sampler.exe" --cases $cases --json "$dir/profile-before.json"
python benchmarks/performance/profile_windows.py --bolt $after --programs "$dir/programs/after" --sampler "$dir/sampler.exe" --cases $cases --json "$dir/profile-after.json"
```

Die Affinitätsmaske 256 verwendet den logischen Prozessor 8 des Messrechners und lässt sich an die verfügbare Hardware anpassen. Der Sampler startet die Programme selbst, tastet ihren Hauptthread ungefähr jede Millisekunde ab und prüft die regulären Ausgaben. Die JSON-Dateien enthalten die Instruktionsadressen relativ zum jeweiligen Modul. Bei kurzen Programmen erhöht der Treiber die Anzahl der Starts, bis mindestens 100 Stichproben vorliegen oder das Startlimit erreicht ist.

Die Kontrollmessung nutzt die reguläre Prozessorplanung in einer neu geöffneten PowerShell. `$cases` und `$dir` bezeichnen dieselbe Programmauswahl und denselben Messordner wie oben. Der Treiber prüft die Fingerabdrücke der vorhandenen Programme und des SDKs vor dem erneuten Start.

```powershell
python benchmarks/performance/release_comparison.py --before build/primitives/bin/bolt.exe --after build/llvm-release/bin/bolt.exe --cases $cases --suite-rounds --reuse-builds "$dir/programs" --build-report "$dir/times.json" --warmup 2 --runs 11 --seed 2941 --json "$dir/confirmation.json"
```

### LLVM Release mit C vergleichen

Der Vergleich von LLVM Release mit C verwendet die 33 Fälle des vorherigen Release-Berichts. Er misst die regulären C-Fassungen und 13 vorab ausgewählte C-Alternativen. Beide C-Summen enthalten einmal dieselben 33 Arbeitslasten. Für die übrigen 20 geht derselbe C-Prozesswert in beide Summen ein. [Gesamtzeiten, Einzelprogramme und C-Varianten](../results/LLVM-Release-vs-C-Windows-x64.md)

```powershell
python benchmarks/performance/c_whole_comparison.py --bolt build/llvm-release/bin/bolt.exe --cc "$env:ProgramFiles/LLVM/bin/clang.exe" --manifest benchmarks/results/llvm-release-whole-confirmation-windows-x64.json --cflag=-march=native --cflag=-ffp-contract=off --pin-cpu 8 --json c-comparison.json
```

Der Aufruf baut die Programme neu. Mit `--bolt-programs` kann ein vorhandenes `after`-Verzeichnis aus der früheren Messung verwendet werden. Der Treiber prüft in diesem Fall die SHA-256-Werte der Bolt-Programme vor dem Start. Ohne `--pin-cpu` läuft ausschließlich die Serie mit regulärer Prozessorplanung.

### Queue-Optimierung mit zwei Bolt-Builds und C messen

Mit `--before` führt der Treiber beide LLVM-Release-Builds und die C-Fassungen desselben Programms unmittelbar nacheinander aus. Das Manifest legt die 33 Programme, Quellen und Sollausgaben fest. Vorher- und Nachher-SDK sind frei wählbar. Die Messdatei enthält die Prüfsummen der tatsächlich verwendeten Compiler, Laufzeiten und Programme. [Queue und Gesamtlaufzeiten](../results/LLVM-Release-Queue-Windows-x64.md)

```powershell
python benchmarks/performance/c_whole_comparison.py --before build/queue-release/before/bin/bolt.exe --bolt build/queue-release/after/bin/bolt.exe --cc "$env:ProgramFiles/LLVM/bin/clang.exe" --manifest benchmarks/results/llvm-release-whole-confirmation-windows-x64.json --cflag=-march=native --cflag=-ffp-contract=off --runs 17 --pinned-runs 15 --pin-cpu 8 --json queue-comparison.json
```

### Memory-Laufzeiten und Aufrufprofile auf Windows x64

Für `memory-long` misst der Treiber beide Bolt-SDKs, die C-Fassung mit `memcmp` und die C-Fassung mit bekanntem gleichem Präfix in denselben Runden. Die Linkerkarte und die COFF-Objekte erlauben anschließend ein Profil genau dieser Bolt-Binärdateien. [Ergebnis für Memory und alle 33 Programme](../results/LLVM-Release-Memory-Windows-x64.md)

```powershell
# In einer x64-Entwicklerkonsole von Visual Studio
$programs=Join-Path $env:TEMP ("bolt-memory-programs-" + [guid]::NewGuid().ToString('N'))
$sampler=Join-Path $env:TEMP 'bolt-memory-sampler.exe'
cl /nologo /O2 /EHsc /std:c++17 benchmarks/performance/windows_sampler.cpp "/Fo$env:TEMP/bolt-memory-sampler.obj" "/Fe:$sampler" winmm.lib psapi.lib
$env:LINK='/MAP'
python benchmarks/performance/c_whole_comparison.py --before build/memory-release/before/bin/bolt.exe --bolt build/memory-release/after/bin/bolt.exe --cc "$env:ProgramFiles/LLVM/bin/clang.exe" --manifest benchmarks/results/llvm-release-whole-confirmation-windows-x64.json --cflag=-march=native --cflag=-ffp-contract=off --runs 15 --pinned-runs 15 --pin-cpu 8 --keep-builds $programs --json memory-times.json
python benchmarks/performance/profile_memory_windows.py --before "$programs/before/memory-long/release/program.exe" --after "$programs/bolt/memory-long/release/program.exe" --sampler $sampler --objdump "$env:ProgramFiles/LLVM/bin/llvm-objdump.exe" --runs 12 --json memory-stacks.json
```

Die Rücksprungadressen im Stack werden anhand der Relokationen des Programmeinstiegs den Speicherprimitiven zugeordnet. So können auch die auf Windows zusammengelegten CRT-Symbole für `memcpy` und `memmove` getrennt ausgewertet werden. In den Programmeinstieg eingebetteter Maschinencode zählt zur Restzeit.

### Buffer und Text gegen die C-Referenzen messen

Der [Windows-x64-Bericht zu Buffer und Text](../results/LLVM-Release-Buffer-Text-Windows-x64.md) vergleicht zwei Builds desselben Quellstands über alle 33 Programme. Aus dem Projektverzeichnis lässt sich die Messung in einer x64 Developer PowerShell wiederholen.

```powershell
python benchmarks/performance/c_whole_comparison.py --before build/buffer-text-release/before/bin/bolt.exe --bolt build/buffer-text-release/after/bin/bolt.exe --cc "$env:ProgramFiles/LLVM/bin/clang.exe" --manifest benchmarks/results/llvm-release-whole-confirmation-windows-x64.json --cflag=-march=native --cflag=-ffp-contract=off --warmup 2 --runs 15 --pin-cpu 8 --pinned-runs 11 --json buffer-text-comparison.json
```

### Langer UTF-8-Rundlauf

Der [Windows-x64-Bericht zum langen UTF-8-Rundlauf](../results/LLVM-Release-UTF8-Rundlauf-Windows-x64.md) vergleicht den vorherigen und den optimierten LLVM-AOT-Build mit zwei C-Fassungen. Eine validiert jeden Snapshot mit SIMD. Die zweite kennt die Position des einzigen Schreibzugriffs. Der Treiber prüft die Ausgabe jedes Starts und erfasst die Aufrufstapel in separaten Läufen. In einer x64 Developer PowerShell lassen sich beide Messreihen wiederholen.

```powershell
$sampler = Join-Path $env:TEMP 'bolt-utf8-sampler.exe'
cl /nologo /O2 /EHsc /std:c++17 benchmarks/performance/windows_sampler.cpp "/Fo$env:TEMP/bolt-utf8-sampler.obj" "/Fe:$sampler" winmm.lib psapi.lib
python benchmarks/performance/utf8_roundtrip_windows.py --before build/utf8-release/before/bin/bolt.exe --after build/utf8-release/after/bin/bolt.exe --cc "$env:ProgramFiles/LLVM/bin/clang.exe" --sampler $sampler --pin-cpu 8 --json utf8-roundtrip.json
```

Für die [Nachmessung mit verfolgter Schreibposition](../results/LLVM-Release-UTF8-Schreibposition-Windows-x64.md) liegen die beiden SDKs unter `build/utf8-write-release/before/bin` und `build/utf8-write-release/after/bin`. Die Wiederholung verwendet zusätzlich `--runs 48 --seed 37061`.

### Einzelne Programmpaare

Dieser Vergleich erzeugt aus derselben Bolt-Quelle je eine Anwendung pro SDK. Beide laufen in balancierter Reihenfolge. Ein unerwarteter Rückgabewert oder eine andere Ausgabe als der feste Sollwert bricht die Messung ab. Die neun Standardfälle decken Listen, Dictionaries, Queue, UTF-8, Buffer und Struct-Speicher ab. Mit `--cases` lassen sich dieselben Arbeitslasten wie in `suite.py` auswählen.

```sh
python3 release_comparison.py --before /opt/bolt-0.15/bin/bolt --after /opt/bolt-0.16/bin/bolt --json old-new.json
```

### List.Insert und RemoveAt nach Listengröße

Der Treiber variiert die Größe des vorhandenen `list-edit`-Falls in temporären Kopien. Er misst den gesamten Prozess sowie die Verarbeitung mit einem Timer um die Schleife. Für Einzelmessungen arbeitet jeder Aufruf auf einer von 64 reservierten Listen. Ein Durchgang fügt in alle Listen ein, liest die zu entfernenden Elemente und löscht sie anschließend. Die beiden Methodenzeiten umfassen jeweils nur ihre eigene Schleife. Die Indexfolge durchläuft über die Messung alle zulässigen Positionen der kurzen Listen. Die C-Referenz verwendet vorab reservierte Arrays und `memmove`.

Nach drei Aufwärmrunden misst der Treiber die Programme in wechselnder Reihenfolge. Er prüft nach jedem Start die Summe der entfernten Elemente und die positionsgewichtete Summe des Listeninhalts gegen eine unabhängige Python-Berechnung. Die JSON-Datei enthält Mediane, Einzelwerte und Operationszahlen. Mit `--before` lässt sich ein zweites Bolt-SDK mitmessen.

```sh
python list-edit-sizes/run.py --bolt /pfad/zum/sdk/bolt --cc clang --sizes 4 8 16 31 64 128 256 1024 32768 --runs 25 --json list-edit.json
```
