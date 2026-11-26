# Ein Bolt-Skript für Funke

Das Beispiel bewegt eine Entity entlang der X-Achse. Die Position gehört der Engine und bleibt über einen Skriptwechsel erhalten. Bolt erhält die Entity-ID und das Frame-Delta von Funke. Die öffentlichen C-Callbacks des Compilers verwenden je zwei 64-Bit-Werte. Das Beispiel überträgt die Zeit in Mikrosekunden und Strecken in Tausendsteln einer Welteinheit.

`BoltSceneScript.hpp` verwendet den originalen Funke-Typ `Funke::NativeEngineAPI`. `headless.cpp` füllt seine Funktionszeiger mit einer kleinen Testwelt. Ein Tick verschiebt die Entity von 2 auf 3. Nach einer Änderung von vier auf acht Einheiten pro Sekunde verschiebt der nächste Tick sie auf 5. Ein Syntaxfehler liefert eine Diagnose. Mit der letzten gültigen Fassung erreicht die Entity anschließend 7. Zeit, Transform und Logging laufen über denselben Header wie in Funke. Der Test läuft ohne Editor und Grafik.

## Auf Linux ausführen

Voraussetzungen sind ein ausgepacktes Bolt-Linux-SDK, ein C++20-Compiler und ein Funke-Checkout. Im Beispielverzeichnis ausführen. Die Variablen bezeichnen absolute Pfade.

```sh
BOLT_SDK=/absoluter/pfad/Bolt-Linux-x64
FUNKE=/absoluter/pfad/Funke
out=$(mktemp -d)
c++ -std=c++20 -O2 -I"$BOLT_SDK/bin/include" -I"$FUNKE/Funke-Engine/src" \
    headless.cpp -L"$BOLT_SDK/bin" -Wl,-rpath,"$BOLT_SDK/bin" -lBoltCompiler -o "$out/funke-smoke"
"$out/funke-smoke" Move.bolt "$out/edited.bolt"
```

## Verwendung im Engine-Projekt

Ein natives Funke-Skript oder ein Scripting-System hält eine Instanz von `BoltSceneScript`. Beim Öffnen und nach einer abgeschlossenen Dateiänderung ruft der Host `reload(absoluterSkriptpfad)` auf. Bei einem Fehler kann er `diagnostic()` im Editor anzeigen. Pro Tick ruft er auf dem Engine-Thread `tick(*Funke::g_EngineAPI, entityID)` auf. Alle Entity-Daten bleiben bei Funke. Die Beispiel-Callbacks laufen synchron. Das Objekt muss so lange leben, bis der letzte Tick und der letzte Reload zurückgekehrt sind.

Das Beispiel initialisiert bei jedem Tick ein Bolt-Programm mit `Main`. Die Kosten dieses Eintrittspunkts pro Entity, der tatsächliche Editor-Frame, das gemeinsame Ausführen mit C#, Input/Scene-APIs und migrationsfähige Bolt-Objekte sind noch zu prüfen. Für tausende Entities ist eine Messung mit einem gemeinsamen Batch-Aufruf vorgesehen.

Getestet mit Funke-Commit `67e570bf2940b975185d0f000aba01aafe6ffbd6`. [Engine-Header](https://github.com/Ben-Scr/Funke/blob/67e570bf2940b975185d0f000aba01aafe6ffbd6/Funke-Engine/src/Scripting/NativeEngineAPI.hpp)
