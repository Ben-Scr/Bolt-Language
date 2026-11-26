# Einen externen C++-Editor mit dem Bolt-SDK bauen

Das Projekt baut den vorhandenen Funke-Testhost gegen die öffentliche C-Schnittstelle eines ausgepackten oder installierten Bolt-SDKs. Dazu gehören das tatsächliche Funke-`NativeEngineAPI`, Skriptwechsel im laufenden Prozess, Callback-Aufrufe mit Entity-ID und der Weiterbetrieb nach einem fehlerhaften Edit.

Du brauchst CMake ab 3.20, einen C++20-Compiler und ein SDK für das Betriebssystem und die Architektur deines Editors. Diese Befehle laufen im Bolt-Quellverzeichnis.

```sh
cmake -S examples/ExternalFunkeHost -B build/funke-host -DBOLT_SDK_ROOT="/Pfad/zu/Bolt"
cmake --build build/funke-host --config Release
ctest --test-dir build/funke-host -C Release --output-on-failure
```

In PowerShell kannst du denselben Aufruf mit einem Windows-SDK verwenden, zum Beispiel `"-DBOLT_SDK_ROOT=$env:LOCALAPPDATA\Programs\Bolt"`. Unter Linux und FreeBSD verwendet das Setup üblicherweise `$HOME/.local/share/bolt`. Bei einem nativen Build reicht auch ein über `PATH` erreichbarer Bolt-Compiler für die Erkennung. Ein Cross-Build braucht das explizite SDK des Ziels sowie die zugehörige CMake-Toolchain.

Der Test erwartet, dass die X-Koordinate einer Entity erst von 2 auf 3, nach einem Skriptwechsel auf 5 und nach einem abgewiesenen Edit auf 7 steigt. Er verwendet das Fast-Debug-Profil des Compilers. `--config Release` optimiert den C++-Host. Bei einem Single-Config-Generator kannst du dafür zusätzlich `-DCMAKE_BUILD_TYPE=Release` beim Konfigurieren setzen. Mit `-DFUNKE_ENGINE_SOURCE_DIR=/pfad/zu/Funke-Engine/src` verwendest du den Header deiner eigenen Engine. Ohne diese Option nutzt das Beispiel die im Simulator enthaltene Fassung.

## In dein Editorprojekt übernehmen

Kopiere `BoltCompilerSDK.cmake` in dein Projekt und binde das bereits definierte CMake-Ziel deines Editors ein.

```cmake
include(cmake/BoltCompilerSDK.cmake)
bolt_import_compiler("${BOLT_SDK_ROOT}")
bolt_link_compiler(FunkeEditor)
```

Das importierte Target `Bolt::Compiler` liefert die Header und die plattformspezifische Bibliothek. Unter Windows verwendet CMake `BoltCompiler.lib` beim Linken und legt `BoltCompiler.dll` sowie die im SDK vorhandene `LLVM-C.dll` neben den Editor. Der Linux- und FreeBSD-Build findet `libBoltCompiler.so` im SDK. Unter macOS liegt `libBoltCompiler.dylib` neben der ausführbaren Datei. Der Mach-O-Loader findet sie über `@executable_path`, auch wenn du beide Dateien gemeinsam verschiebst. Bei Linux und FreeBSD zeigt der Build-RPATH auf das SDK. Für die Weitergabe eines Editors wählt deine Anwendung ihren Installationsort und Install-RPATH.
