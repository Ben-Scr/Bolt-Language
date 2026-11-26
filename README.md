# Bolt

Bolt is a statically typed language for native programs and embedded scripts. This tree includes the compiler sources, standard library and a Windows x64 compiler.

**[Read the Bolt documentation](docs/README.md)** · [Build your first project](docs/Getting-Started.md) · [Language reference](docs/Language.md) · [Library API](docs/Library-API.md) · [Embed Bolt in an engine](docs/Embedding.md)

```powershell
.\bin\bolt.exe version
.\bin\bolt.exe run tests/native/list-add-range.bolt --release
```

In VS Code lässt sich derselbe Compiler über **Bolt: Select Compiler…** auswählen. Das installierbare Paket und die Bedienungsanleitung liegen unter [editors/vscode](editors/vscode/).

Für Visual Studio 2022 ab 17.14 und Visual Studio 2026 gibt es ein gemeinsames Windows-x64-VSIX mit IntelliSense, Projektwerkzeugen und nativem Debugging. [Installation und Bedienung](editors/visualstudio/README.md)

Neue Bolt-Projekte lassen sich direkt in der IDE erstellen. In Visual Studio unter **Neues Projekt erstellen** die Vorlage **Bolt Console Application** wählen, in IntelliJ IDEA unter **New Project** den Eintrag **Bolt**. [IntelliJ-Plugin und Anleitung](docs/Editors.md#intellij-idea)

## Lambdas und Closures

`Action<T...>` beschreibt einen Callback ohne Rückgabewert. Bei `Func<T..., TResult>` ist das letzte Typargument der Rückgabetyp. Beide lassen sich direkt mit Klammern aufrufen. Ein Lambda kann einen Ausdruck oder einen Anweisungsblock enthalten.

```bolt
int total = 0;
Action<int> add = amount => { total += amount; };
Func<int, int> plusTotal = x => x + total;
add(3);
Print(plusTotal(4));  // 7
```

Veränderliche lokale Variablen und Wertparameter teilen ihren Speicher mit den Closures, die sie verwenden. Eine zurückgegebene Closure hält diese Variablen am Leben. Jeder erneute Aufruf der erzeugenden Funktion erhält eigene Variablen. Ein Event-Handler lässt sich wieder abmelden, indem man die gespeicherte `Action` an `-=` übergibt. Für einen Block oder für Parameter ohne Typangabe liefert das Ziel den Delegattyp, etwa bei einer Zuweisung, einem Funktionsargument oder einer Rückgabe. Bei einem Ausdruck wie `var square = (int x) => x * x;` kann Bolt den Typ ableiten.

Greift ein Lambda in einer Instanzmethode auf `this` oder ein Instanzmitglied zu, übernimmt es eine Kopie des Empfängers. Kopien desselben Delegaten teilen diesen Zustand. Für lokale Pointer, Referenzvariablen und Arena-Besitzer meldet der Compiler einen Fehler beim Einfangen.

## Switch

```bolt
switch (state) {
    case State.Run:
    case State.Jump:
        Animate();
        break;
    default:
        StopAnimation();
}
```

Der Ausdruck hinter `switch` wird einmal ausgewertet. Ganzzahlen, Enums, Zeichen, Strings und boolesche Werte sind als Selektoren zulässig. Aufeinanderfolgende Labels teilen einen Zweig. Am Ende eines Zweigs geht es hinter dem `switch` weiter. Ein ausdrückliches `break` kann den Zweig vorzeitig verlassen. `continue` bezieht sich auf die umgebende Schleife. `default` ist optional und darf auch vor einem `case` stehen.

## Async, Reflection und JSON

Ein `async Task<int> Main(string[] args)` kann im Editor per JIT laufen und als natives Release-Programm gebaut werden. Beide Wege warten auf den Task und verwenden dessen Ergebnis als Exitcode. Das Beispiel liest und schreibt JSON über asynchrone Dateioperationen und serialisiert die Properties einer mit `[Reflect]` markierten Klasse.

```powershell
.\bin\bolt.exe run examples/Stdlib11/AsyncJson/Program.bolt --jit --require-native
.\bin\bolt.exe run examples/Stdlib11/AsyncJson/Program.bolt --release --require-native
```

## Einbettung in Funke und andere Hosts

Die Compiler-Host-Schnittstelle in `include/bolt/CompilerHost.h` unterstützt mehrere benannte Quelltexte im Speicher. Ein Host kann damit seine Bibliothek und einen eigenen Einstiegspunkt ergänzen. Fehler verweisen auf den jeweiligen Quelldateinamen und die ursprüngliche Zeile.

`bolt_compiler_run_persistent` hält den Zustand eines geladenen Moduls über mehrere Aufrufe. Funke nutzt das für die Felder seiner Skriptinstanzen. Ein neu kompiliertes Ersatzmodul beginnt mit frischem Zustand. `bolt_compiler_run` führt Module weiterhin mit frischem Zustand pro Aufruf aus. `bolt_compiler_visit_fields` liefert Felddeklarationen und Attribute an Werkzeuge, ohne den Benutzerquelltext auszuführen. Freie Funktionen, prozedurale Programme und die bisherigen Kommandozeileneinstiege bleiben verfügbar.

## Standardbibliothek und Benchmarks

Die Implementierung von `List<T>.AddRange` steht in [List.bolt](stdlib/System/Collections/Generic/List.bolt). CMake bettet die Standardbibliothek beim Bauen des Compilers ein. Der [Regressionstest](tests/native/list-add-range.bolt) prüft unter anderem das Anhängen an sich selbst, verschachtelte Listen, Wertkopien und die Gültigkeit von Iteratoren.

Der [Laufzeitbenchmark](benchmarks/performance/cases/list-add-range.bolt) hängt 32.768-mal eine Liste mit 131.072 Ganzzahlen an eine vorreservierte Zielliste an. Für den Vorher-Nachher-Vergleich übersetzt jeder Compiler dasselbe Programm mit `--release --backend llvm --require-native`. [compare.py](benchmarks/performance/compare.py) startet die beiden Programme in wechselnder Reihenfolge und prüft ihre Ausgabe. Die [Einzelmessungen](benchmarks/results/list-add-range-windows-x64.json) enthalten die Laufzeiten einschließlich Prozessstart.

Für `Dictionary.Clear()` und `HashSet.Clear()` räumt [HashTable.bolt](stdlib/System/Collections/Generic/HashTable.bolt) dünn belegte Tabellen anhand der gespeicherten Hashwerte auf. Der [Dictionary-Benchmark](benchmarks/performance/cases/dictionary-clear.bolt) nimmt reservierte Kapazität, Anzahl der Einträge pro Runde und Rundenzahl als Argumente. Er misst Einfügen, Nachschlagen und Leeren zusammen. Die [Windows-x64-Messreihe](benchmarks/results/dictionary-clear-windows-x64.json) vergleicht dünne und dichte Belegung mit dem vorherigen lokalen Compiler.
