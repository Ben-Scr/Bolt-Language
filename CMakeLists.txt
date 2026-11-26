cmake_minimum_required(VERSION 3.21)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/BoltAppleTarget.cmake")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/bolt/Version.hpp" BOLT_VERSION_DEFINE REGEX "^#define BOLT_VERSION_STRING ")
string(REGEX MATCH "[0-9]+[.][0-9]+[.][0-9]+" BOLT_RELEASE_VERSION "${BOLT_VERSION_DEFINE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/bolt/Version.hpp")
project(Bolt VERSION ${BOLT_RELEASE_VERSION} LANGUAGES CXX)
if(APPLE)
    add_compile_definitions(BOLT_MACOS_DEPLOYMENT_TARGET="${CMAKE_OSX_DEPLOYMENT_TARGET}")
    message(STATUS "Bolt compiler/runtime/application deployment target: macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(BOLT_LINUX_MUSL "Build the native compiler and its programs for the musl ABI" OFF)
    if(BOLT_LINUX_MUSL)
        add_compile_definitions(BOLT_LINUX_MUSL=1)
    endif()
endif()
option(BOLT_SANITIZE "Enable address/undefined sanitizers (Clang/GCC)" OFF)
if(BOLT_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "BOLT_SANITIZE currently requires Clang/GCC")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
option(BOLT_BUILD_TESTS "Build regression tests" ON)
option(BOLT_BUILD_EMBEDDING "Build the C++ embedding example" ON)
set(BOLT_CORE_SOURCES
    src/Preprocessor.cpp src/Project.cpp src/Tooling.cpp src/Diagnostics.cpp src/Ast.cpp src/Lexer.cpp src/Parser.cpp src/ParseCache.cpp src/Semantic.cpp src/Value.cpp
    src/semantic/BuiltinCalls.cpp src/semantic/Calls.cpp src/semantic/Concepts.cpp src/semantic/Conversions.cpp
    src/semantic/Declarations.cpp src/semantic/Delegates.cpp src/semantic/Expressions.cpp src/semantic/Flow.cpp
    src/semantic/MemberBinding.cpp src/semantic/Navigation.cpp src/semantic/Overloads.cpp
    src/semantic/Statements.cpp src/semantic/Metadata.cpp src/semantic/Substitution.cpp src/semantic/TypeResolution.cpp
    src/Interpreter.cpp src/Emitter.cpp src/StandardLibrary.cpp
    src/native/IR.cpp src/native/LLVM.cpp src/native/Fast.cpp src/native/FastAarch64.cpp src/native/Object.cpp src/native/Dwarf.cpp src/native/WindowsArm64.cpp src/native/CoffArm64Jit.cpp
    src/native/Capabilities.cpp src/native/LibrarySize.cpp src/native/Lower.cpp src/native/ABI.cpp src/native/Jit.cpp
    src/native/lowering/Builder.cpp src/native/lowering/Calls.cpp src/native/lowering/Expressions.cpp
    src/native/lowering/Layout.cpp src/native/lowering/LoopOptimizations.cpp
    src/native/lowering/Statements.cpp src/native/lowering/StorageOptimizations.cpp src/native/lowering/Values.cpp)
# Source-authored libraries are input to the compiler, not hand-maintained signatures.
set_property(SOURCE src/StandardLibrary.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/stdlib/Embedded.inc")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    file(GLOB_RECURSE BOLT_LIBRARY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/*.bolt")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/src/stdlib/Embedded.inc"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-stdlib.py"
        DEPENDS ${BOLT_LIBRARY_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-stdlib.py"
        VERBATIM)
    add_custom_target(BoltEmbeddedLibrary DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/stdlib/Embedded.inc")
endif()
find_package(Threads REQUIRED)
add_library(BoltObjects OBJECT ${BOLT_CORE_SOURCES})
if(TARGET BoltEmbeddedLibrary)
    add_dependencies(BoltObjects BoltEmbeddedLibrary)
endif()
set_target_properties(BoltObjects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(BoltObjects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(BoltObjects PUBLIC cxx_std_20)
add_library(BoltCore STATIC $<TARGET_OBJECTS:BoltObjects>)
if(MSVC)
    # The program-facing DLL exposes a C ABI and owns its allocations. Keep its
    # CRT inside the DLL so launching a script doesn't load the C++ redist DLLs.
    # BoltCore retains the toolchain CRT used by C++ embedding applications.
    add_library(BoltRuntime SHARED ${BOLT_CORE_SOURCES})
    set_property(TARGET BoltRuntime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(BoltRuntime PRIVATE /W4 /permissive- /utf-8)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$" AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "[Aa][Rr][Mm]")
        add_library(BoltAotRuntime STATIC src/native/WindowsMain.cpp src/native/WindowsConsole.cpp src/native/ABI.cpp src/Diagnostics.cpp src/native/Capabilities.cpp)
        target_include_directories(BoltAotRuntime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_compile_definitions(BoltAotRuntime PRIVATE BOLT_WINDOWS_AOT_RUNTIME)
        target_compile_options(BoltAotRuntime PRIVATE /Gy /Gw /bigobj /utf-8)
        set_property(TARGET BoltAotRuntime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # Debug archives use the debug CRT and are not suitable for shipping Release scripts.
        set_target_properties(BoltAotRuntime PROPERTIES DEBUG_POSTFIX "d")
        install(TARGETS BoltAotRuntime ARCHIVE DESTINATION bin CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
    endif()
    if(TARGET BoltEmbeddedLibrary)
        add_dependencies(BoltRuntime BoltEmbeddedLibrary)
    endif()
else()
    add_library(BoltRuntime SHARED $<TARGET_OBJECTS:BoltObjects>)
endif()
foreach(target BoltCore BoltRuntime)
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_link_libraries(${target} PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endforeach()
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_link_libraries(BoltCore PUBLIC execinfo)
    target_link_libraries(BoltRuntime PRIVATE execinfo)
endif()
if(WIN32)
    target_link_libraries(BoltCore PUBLIC shell32 ws2_32)
    target_link_libraries(BoltRuntime PRIVATE shell32 ws2_32)
    if(MSVC)
        # Most native programs use neither Windows shell argument parsing nor sockets.
        target_link_libraries(BoltRuntime PRIVATE delayimp)
        target_link_options(BoltRuntime PRIVATE /DELAYLOAD:shell32.dll /DELAYLOAD:ws2_32.dll)
        if(MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64")
            # LLVM emits this CRT helper for large Windows stack frames.
            # AOT executables link only the runtime's import library.
            target_link_options(BoltRuntime PRIVATE /EXPORT:__chkstk)
        endif()
    endif()
    set_target_properties(BoltRuntime PROPERTIES PREFIX "")
elseif(APPLE)
    set_target_properties(BoltRuntime PROPERTIES INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
    target_link_options(BoltRuntime PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/native/MacRuntime.exports")
    set_property(TARGET BoltRuntime APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/MacRuntime.exports")
endif()
add_library(BoltCompiler SHARED src/CompilerHost.cpp)
target_compile_definitions(BoltCompiler PRIVATE BOLT_COMPILER_BUILD)
target_link_libraries(BoltCompiler PRIVATE BoltCore)
target_include_directories(BoltCompiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(WIN32)
    set_target_properties(BoltCompiler PROPERTIES PREFIX "")
elseif(BOLT_LINUX_MUSL)
    target_link_options(BoltCompiler PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/native/ElfCompiler.map")
    set_property(TARGET BoltCompiler APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/ElfCompiler.map")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_link_options(BoltCompiler PRIVATE "LINKER:--exclude-libs,ALL")
elseif(APPLE)
    set_target_properties(BoltCompiler PROPERTIES INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
    target_link_options(BoltCompiler PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/native/MacCompiler.exports")
    set_property(TARGET BoltCompiler APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/MacCompiler.exports")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    enable_language(ASM)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        set(BOLT_START_SOURCE src/native/Start-x86_64.S)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(BOLT_START_SOURCE src/native/Start-aarch64.S)
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "riscv64")
        set(BOLT_START_SOURCE src/native/Start-riscv64.S)
    else()
        message(FATAL_ERROR "Unsupported Linux architecture for Bolt program startup")
    endif()
    add_library(BoltStart OBJECT ${BOLT_START_SOURCE})
    if(BOLT_LINUX_MUSL)
        enable_language(C)
        add_library(BoltStartUnwind OBJECT src/native/DiagnosticUnwind.c)
        target_include_directories(BoltStartUnwind PRIVATE include)
        target_compile_options(BoltStartUnwind PRIVATE -fPIC -ffunction-sections -funwind-tables)
        set(bolt_start_object "${CMAKE_CURRENT_BINARY_DIR}/BoltStart.o")
        add_custom_command(OUTPUT "${bolt_start_object}"
            COMMAND "${CMAKE_LINKER}" -r $<TARGET_OBJECTS:BoltStart> $<TARGET_OBJECTS:BoltStartUnwind> -o "${bolt_start_object}"
            DEPENDS BoltStart BoltStartUnwind $<TARGET_OBJECTS:BoltStart> $<TARGET_OBJECTS:BoltStartUnwind> COMMAND_EXPAND_LISTS)
    else()
        set(bolt_start_object $<TARGET_OBJECTS:BoltStart>)
    endif()
    add_custom_target(BoltStartArtifact ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:BoltRuntime>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${bolt_start_object} "$<TARGET_FILE_DIR:BoltRuntime>/BoltStart.o"
        DEPENDS BoltStart BoltRuntime ${bolt_start_object} COMMAND_EXPAND_LISTS)
    install(FILES ${bolt_start_object} DESTINATION bin RENAME BoltStart.o)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND Python3_Interpreter_FOUND AND NOT BOLT_SANITIZE)
        target_compile_options(BoltObjects PRIVATE -ffunction-sections -fdata-sections)
        if(CMAKE_CONFIGURATION_TYPES)
            set(aot_runtime "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/BoltAotRuntime.o")
        else()
            set(aot_runtime "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/BoltAotRuntime.o")
        endif()
        add_custom_command(OUTPUT "${aot_runtime}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make-aot-runtime.py"
                    "${CMAKE_CXX_COMPILER}" "$<TARGET_FILE:BoltCore>" "${aot_runtime}"
            DEPENDS BoltCore "${CMAKE_CURRENT_SOURCE_DIR}/tools/make-aot-runtime.py" VERBATIM)
        add_custom_target(BoltAotRuntime ALL DEPENDS "${aot_runtime}")
        install(FILES "${aot_runtime}" DESTINATION bin)
    endif()
endif()
add_executable(Bolt src/Main.cpp src/tooling/LanguageServer.cpp)
set_target_properties(Bolt PROPERTIES OUTPUT_NAME bolt)
target_link_libraries(Bolt PRIVATE BoltCore)
add_dependencies(Bolt BoltRuntime)
if(TARGET BoltAotRuntime)
    add_dependencies(Bolt BoltAotRuntime)
endif()
if(TARGET BoltStartArtifact)
    add_dependencies(Bolt BoltStartArtifact)
endif()
target_compile_definitions(Bolt PRIVATE BOLT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include")
if(MSVC)
    target_compile_options(BoltObjects PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(Bolt PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(BoltObjects PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(Bolt PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(MINGW)
    target_link_options(Bolt PRIVATE -municode)
endif()
if(BOLT_BUILD_TESTS)
    enable_testing()
    if(Python3_Interpreter_FOUND)
        add_test(NAME Bolt.CompilerHost COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler_host.py" "$<TARGET_FILE:BoltCompiler>")
        set_tests_properties(Bolt.CompilerHost PROPERTIES TIMEOUT 240)
        if(Python3_VERSION VERSION_GREATER_EQUAL "3.11")
            add_test(NAME Bolt.Watcher COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/bolt_watch.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/bolt-watch.py" --library "$<TARGET_FILE:BoltCompiler>")
            set_tests_properties(Bolt.Watcher PROPERTIES TIMEOUT 240)
        endif()
        add_test(NAME Bolt.SecureRandom COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/secure_random.py" --bolt "$<TARGET_FILE:Bolt>")
        add_test(NAME Bolt.IntegerCastBoundaries COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/integer_casts.py" "$<TARGET_FILE:Bolt>" --fuzz 4)
        set_tests_properties(Bolt.IntegerCastBoundaries PROPERTIES TIMEOUT 240)
    endif()
    if(WIN32)
        add_executable(BoltWindowsAdjacentLLVM tests/WindowsAdjacentLLVM.cpp)
        target_include_directories(BoltWindowsAdjacentLLVM PRIVATE include)
        target_compile_features(BoltWindowsAdjacentLLVM PRIVATE cxx_std_20)
        if(MINGW)
            target_link_options(BoltWindowsAdjacentLLVM PRIVATE -municode)
        endif()
        set_target_properties(BoltWindowsAdjacentLLVM PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/isolated-editor")
        add_test(NAME Bolt.WindowsAdjacentLLVM COMMAND BoltWindowsAdjacentLLVM "$<TARGET_FILE:BoltCompiler>")
        set_tests_properties(Bolt.WindowsAdjacentLLVM PROPERTIES SKIP_RETURN_CODE 77)
        find_program(BOLT_POWERSHELL NAMES powershell.exe)
        if(BOLT_POWERSHELL)
            add_test(NAME Bolt.WindowsDependencies
                COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath
                    "${BOLT_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_dependencies.ps1"
                -Bolt "$<TARGET_FILE:Bolt>")
            set_tests_properties(Bolt.WindowsDependencies PROPERTIES TIMEOUT 180)
        endif()
    endif()
    add_executable(BoltTests tests/Tests.cpp)
    target_link_libraries(BoltTests PRIVATE BoltCore)
    add_test(NAME Bolt.Unit COMMAND BoltTests)
    add_executable(BoltNativeTests tests/NativeTests.cpp)
    target_link_libraries(BoltNativeTests PRIVATE BoltCore)
    add_test(NAME Bolt.NativeUnit COMMAND BoltNativeTests)
    add_executable(BoltFastCodegenTests tests/FastCodegenTests.cpp)
    target_link_libraries(BoltFastCodegenTests PRIVATE BoltCore)
    add_test(NAME Bolt.FastCodegen COMMAND BoltFastCodegenTests)
    add_executable(BoltArm64CoffRelocations tests/CoffArm64Relocations.cpp)
    target_compile_features(BoltArm64CoffRelocations PRIVATE cxx_std_20)
    add_test(NAME Bolt.Arm64CoffRelocations COMMAND BoltArm64CoffRelocations)
    if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64|AARCH64)$")
        add_executable(BoltWindowsArm64LLVM tests/WindowsArm64LLVM.cpp)
        target_link_libraries(BoltWindowsArm64LLVM PRIVATE BoltCore)
        add_test(NAME Bolt.WindowsArm64LLVM COMMAND BoltWindowsArm64LLVM)
        set_tests_properties(Bolt.WindowsArm64LLVM PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(BoltArmJitAddresses tests/ArmJitAddresses.cpp)
    target_link_libraries(BoltArmJitAddresses PRIVATE BoltCore)
    add_test(NAME Bolt.ArmJitAddresses COMMAND BoltArmJitAddresses)
    add_executable(BoltWindowsArm64UnwindTests tests/WindowsArm64Unwind.cpp)
    target_link_libraries(BoltWindowsArm64UnwindTests PRIVATE BoltCore)
    add_test(NAME Bolt.WindowsArm64Unwind COMMAND BoltWindowsArm64UnwindTests)
    if(WIN32 AND Python3_Interpreter_FOUND AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_test(NAME Bolt.WindowsAotStackwalk COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_aot_stackwalk.py"
            --emitter "$<TARGET_FILE:BoltWindowsArm64UnwindTests>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.WindowsAotStackwalk PROPERTIES TIMEOUT 180)
    endif()
    set_tests_properties(Bolt.NativeUnit PROPERTIES TIMEOUT 180)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        if(WIN32)
            add_test(NAME Bolt.WindowsNativeConsole
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_native_console.py"
                --bolt "$<TARGET_FILE:Bolt>" --work "${CMAKE_BINARY_DIR}/windows-native-console")
            set_tests_properties(Bolt.WindowsNativeConsole PROPERTIES TIMEOUT 300)
        endif()
        if(UNIX)
            add_test(NAME Bolt.POSIXNativeConsole
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/posix_native_console.py"
                --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}"
                --work "${CMAKE_BINARY_DIR}/posix-native-console")
            set_tests_properties(Bolt.POSIXNativeConsole PROPERTIES TIMEOUT 600)
        endif()
        if(WIN32 OR UNIX)
            add_test(NAME Bolt.NativeDateTime
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_datetime.py"
                --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}"
                --work "${CMAKE_BINARY_DIR}/native-datetime")
            set_tests_properties(Bolt.NativeDateTime PROPERTIES TIMEOUT 600)
        endif()
        add_test(NAME Bolt.LanguageFeatures05
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/v5_features.py"
            --bolt "$<TARGET_FILE:Bolt>")
        set_tests_properties(Bolt.LanguageFeatures05 PROPERTIES TIMEOUT 180)
        if(NOT BOLT_SANITIZE)
            add_test(NAME Bolt.NativeParity
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_parity.py"
                --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}"
                --work "${CMAKE_BINARY_DIR}/parity")
            set_tests_properties(Bolt.NativeParity PROPERTIES TIMEOUT 1200)
            add_test(NAME Bolt.DirectParity
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/direct_parity.py"
                --bolt "$<TARGET_FILE:Bolt>" --work "${CMAKE_BINARY_DIR}/direct-parity")
            set_tests_properties(Bolt.DirectParity PROPERTIES TIMEOUT 900)
            add_test(NAME Bolt.NativeCLI
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_cli.py"
                --bolt "$<TARGET_FILE:Bolt>")
            set_tests_properties(Bolt.NativeCLI PROPERTIES TIMEOUT 180)
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
               CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|aarch64|arm64|ARM64)$")
                add_test(NAME Bolt.LinuxCrossObjects COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/linux_cross.py" --bolt "$<TARGET_FILE:Bolt>")
                find_program(BOLT_ADDR2LINE addr2line)
                find_program(BOLT_READELF readelf)
                find_program(BOLT_NM nm)
                if(BOLT_ADDR2LINE AND BOLT_READELF AND BOLT_NM)
                    add_test(NAME Bolt.FastDebugLines COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fast_debug_lines.py" --bolt "$<TARGET_FILE:Bolt>"
                        --addr2line "${BOLT_ADDR2LINE}" --readelf "${BOLT_READELF}" --nm "${BOLT_NM}")
                endif()
            endif()

            add_test(NAME Bolt.ProjectCLI
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/project_cli.py"
                --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}"
                --work "${CMAKE_BINARY_DIR}/project-cli")
            set_tests_properties(Bolt.ProjectCLI PROPERTIES TIMEOUT 300)
        endif()
        add_test(NAME Bolt.CLIPaths
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_paths.py"
            --bolt "$<TARGET_FILE:Bolt>" --work "${CMAKE_BINARY_DIR}/cli-path-tests")
        add_test(NAME Bolt.LanguageServer
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_protocol.py"
            --bolt "$<TARGET_FILE:Bolt>")
        set_tests_properties(Bolt.LanguageServer PROPERTIES TIMEOUT 120)
        add_test(NAME Bolt.LanguageServerWorkspaceEdits
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_workspace_edits.py"
            "$<TARGET_FILE:Bolt>")
        set_tests_properties(Bolt.LanguageServerWorkspaceEdits PROPERTIES TIMEOUT 120)
        add_test(NAME Bolt.VariadicGenerics
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/variadic_generics.py"
            --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.VariadicGenerics PROPERTIES TIMEOUT 450)
        add_test(NAME Bolt.SemanticNavigation
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_navigation.py"
            --bolt "$<TARGET_FILE:Bolt>")
        set_tests_properties(Bolt.SemanticNavigation PROPERTIES TIMEOUT 120)
        add_test(NAME Bolt.TypeVisibility
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/type_visibility.py"
            --bolt "$<TARGET_FILE:Bolt>")
        add_test(NAME Bolt.NestedTypes
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nested_types.py"
            --bolt "$<TARGET_FILE:Bolt>")
        add_test(NAME Bolt.TypeVisibilityEditor
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/type_visibility_editor.py"
            --bolt "$<TARGET_FILE:Bolt>")
        set_tests_properties(Bolt.TypeVisibility Bolt.NestedTypes Bolt.TypeVisibilityEditor PROPERTIES TIMEOUT 180)
        if(NOT BOLT_SANITIZE)
            add_test(NAME Bolt.NestedBackends
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nested_backends.py"
                --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
            add_test(NAME Bolt.OperatorDiagnostics
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/operator_diagnostics.py"
                --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
            set_tests_properties(Bolt.NestedBackends Bolt.OperatorDiagnostics PROPERTIES TIMEOUT 600)
        endif()
        add_test(NAME Bolt.IntelliSense
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_intellisense.py"
            --bolt "$<TARGET_FILE:Bolt>")
        add_test(NAME Bolt.IntelliJCompletion
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/intellij_completion.py"
            --bolt "$<TARGET_FILE:Bolt>")
        set_tests_properties(Bolt.IntelliSense Bolt.IntelliJCompletion PROPERTIES TIMEOUT 120)

        if(BOLT_SANITIZE)
            add_test(NAME Bolt.ToolingCLI
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling_cli.py"
                --bolt "$<TARGET_FILE:Bolt>" --skip-native)
        else()
            add_test(NAME Bolt.ToolingCLI
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling_cli.py"
                --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
            if(UNIX)
                add_test(NAME Bolt.InstallPOSIX
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_posix.py"
                    --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
                set_tests_properties(Bolt.InstallPOSIX PROPERTIES TIMEOUT 180)
            endif()
        endif()
        set_tests_properties(Bolt.ToolingCLI PROPERTIES TIMEOUT 180)
        add_test(NAME Bolt.IntelliJStructure
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_editor.py")
    endif()
endif()
if(BOLT_BUILD_EMBEDDING)
    add_executable(BoltEmbedding examples/embedding/main.cpp)
    target_link_libraries(BoltEmbedding PRIVATE BoltCore)
    if(BOLT_BUILD_TESTS)
        add_test(NAME Bolt.Embedding COMMAND BoltEmbedding)
    endif()
endif()

# Keep the archive and headers beside the CLI so an installed tree is movable.
install(TARGETS Bolt BoltCore BoltRuntime BoltCompiler RUNTIME DESTINATION bin ARCHIVE DESTINATION bin LIBRARY DESTINATION bin)
install(DIRECTORY include/bolt DESTINATION bin/include)
install(PROGRAMS tools/bolt-watch.py DESTINATION bin)
install(FILES README.md LICENSE DESTINATION share/bolt)

# 0.7 end-to-end source/library regressions are intentionally separate from the
# legacy direct-backend corpus: advanced library features select native C++.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.LibraryEditor
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/library_editor.py"
        --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.LibraryEditor PROPERTIES TIMEOUT 120)
    if(NOT BOLT_SANITIZE)
        add_test(NAME Bolt.LibraryRuntime
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/library_runtime.py"
            --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.LibraryRuntime PROPERTIES TIMEOUT 900)
    endif()
endif()

install(DIRECTORY docs/licenses DESTINATION share/bolt)
install(DIRECTORY stdlib DESTINATION share/bolt)
# Remove the previous SDK declarations when upgrading an existing prefix.
# Keeping them would introduce duplicate Bolt types in the installed stdlib.
install(CODE [[
    foreach(old IN ITEMS
        share/bolt/stdlib/System/StandaloneProcess.bolt
        share/bolt/stdlib/System/Net/StandaloneAddresses.bolt
        share/bolt/stdlib/System/Net/StandaloneSocket.bolt
        share/bolt/stdlib/Compatibility/PlatformTasks.bolt
        share/bolt/stdlib/Compatibility/PlatformThreading.bolt
        share/bolt/stdlib/Compatibility/Synchronization.bolt
        bin/include/bolt/TaskState.hpp
        bin/include/bolt/SystemThreading.hpp
        bin/include/bolt/library/Task.hpp)
        file(REMOVE "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${old}")
    endforeach()
]])


# 0.8: tolerant editor analysis and the precompiled direct library ABI.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.EditorResilience
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/editor_resilience.py"
        --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.EditorResilience PROPERTIES TIMEOUT 120)
    if(NOT BOLT_SANITIZE)
        add_test(NAME Bolt.SharedRuntime
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_runtime.py"
            --bolt "$<TARGET_FILE:Bolt>")
        set_tests_properties(Bolt.SharedRuntime PROPERTIES TIMEOUT 300)
    endif()
endif()

# 0.9 first-class properties, ordinary-source containers and extension migration.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.PropertiesEditor COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nine_editor.py" --bolt "$<TARGET_FILE:Bolt>")
    if(NOT BOLT_SANITIZE)
        add_test(NAME Bolt.SourceLibrary09 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nine_runtime.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.SourceLibrary09 PROPERTIES TIMEOUT 900)
    endif()
endif()

# 0.10: native AOT is the CLI default; saved builds never consult source/LLVM.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND AND NOT BOLT_SANITIZE)
    add_test(NAME Bolt.RunModes
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_modes.py"
        --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
    set_tests_properties(Bolt.RunModes PROPERTIES TIMEOUT 900)
    add_test(NAME Bolt.ExternalBuildRoot
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/external_build_root.py"
            --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
    set_tests_properties(Bolt.ExternalBuildRoot PROPERTIES TIMEOUT 480)
    add_test(NAME Bolt.CppCacheInputs COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp_cache_inputs.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
    set_tests_properties(Bolt.CppCacheInputs PROPERTIES TIMEOUT 600)
    if(NOT MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_test(NAME Bolt.CppPchInputs COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp_pch_inputs.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.CppPchInputs PROPERTIES TIMEOUT 600)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|aarch64|arm64|ARM64)$")
        add_test(NAME Bolt.CppDebugLines COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp_debug_lines.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.CppDebugLines PROPERTIES TIMEOUT 300)
        add_test(NAME Bolt.NativeCacheInputs COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_cache_inputs.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.NativeCacheInputs PROPERTIES TIMEOUT 360)
    endif()
endif()

# 0.11 source-authored I/O/conversion/Task/JSON and Apple target compatibility.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.Migration11Editor COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/migration11_editor.py" --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.Migration11Editor PROPERTIES TIMEOUT 120)
    if(NOT BOLT_SANITIZE)
        add_test(NAME Bolt.Migration11 COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/migration11_runtime.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.Migration11 PROPERTIES TIMEOUT 900)
        add_test(NAME Bolt.AppleTarget COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/apple_target.py" --cxx "${CMAKE_CXX_COMPILER}" --cmake "${CMAKE_COMMAND}")
        set_tests_properties(Bolt.AppleTarget PROPERTIES TIMEOUT 120)
    endif()
endif()

# 0.12 shared project resolution and symbol-based editor colors.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.Concepts12Editor COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/concepts12_editor.py" --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.Concepts12Editor PROPERTIES TIMEOUT 180)
    if(NOT BOLT_SANITIZE)
        add_test(NAME Bolt.Projects12 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/projects12.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.Projects12 PROPERTIES TIMEOUT 900)
        add_test(NAME Bolt.NativeProjectLibraries COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_project_libraries.py" --bolt "$<TARGET_FILE:Bolt>")
        set_tests_properties(Bolt.NativeProjectLibraries PROPERTIES TIMEOUT 300)
    endif()
endif()

# 0.13: conditional compilation, semantic attributes and optional values.
if(BOLT_BUILD_TESTS)
    add_executable(BoltPreprocessorTests tests/Preprocessor13.cpp)
    target_link_libraries(BoltPreprocessorTests PRIVATE BoltCore)
    add_test(NAME Bolt.Preprocessor13 COMMAND BoltPreprocessorTests)
endif()
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND AND NOT BOLT_SANITIZE)
    add_test(NAME Bolt.Optional13 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/optional13.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
    add_test(NAME Bolt.PreprocessorCLI13 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/preprocessor13_cli.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
    set_tests_properties(Bolt.Optional13 Bolt.PreprocessorCLI13 PROPERTIES TIMEOUT 900)
endif()
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.Attributes13 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/attributes13_editor.py" --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.Attributes13 PROPERTIES TIMEOUT 180)
endif()

# 0.14 systems library, direct cleanup/exception lowering and attribute completion.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.AttributeCompletion14 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/attributes14_editor.py" --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.AttributeCompletion14 PROPERTIES TIMEOUT 120)
    if(NOT BOLT_SANITIZE)
        add_test(NAME Bolt.Systems14 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/systems14_runtime.py" --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.Systems14 PROPERTIES TIMEOUT 900)
    endif()
endif()

if(BOLT_BUILD_TESTS)
    add_executable(BoltSystemPrimitivesTests tests/SystemPrimitives14.cpp)
    target_include_directories(BoltSystemPrimitivesTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(BoltSystemPrimitivesTests PRIVATE cxx_std_20)
    target_link_libraries(BoltSystemPrimitivesTests PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(BoltSystemPrimitivesTests PRIVATE ws2_32)
    endif()
    add_test(NAME Bolt.SystemPrimitives14 COMMAND BoltSystemPrimitivesTests)
    set_tests_properties(Bolt.SystemPrimitives14 PROPERTIES TIMEOUT 120)
endif()

# 0.15: real events/conversions, semantic editor queries and severity records.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.EventsConversions15
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/features15_runtime.py"
        --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
    add_test(NAME Bolt.ClosuresSwitch
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/closures_switch.py"
        --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
    add_test(NAME Bolt.EditorFeatures15
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/features15_editor.py"
        --bolt "$<TARGET_FILE:Bolt>")
    add_test(NAME Bolt.HoverSize
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/hover_size.py"
        --bolt "$<TARGET_FILE:Bolt>")
    add_test(NAME Bolt.ConsoleSeverity15
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/console15_cli.py"
        --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.EventsConversions15 PROPERTIES TIMEOUT 480)
    set_tests_properties(Bolt.EditorFeatures15 Bolt.HoverSize Bolt.ConsoleSeverity15 PROPERTIES TIMEOUT 180)
endif()

# Shared structural iteration and source-defined extension methods.
if(BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME Bolt.ExtensionEditor
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_extensions.py"
        --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.ExtensionEditor PROPERTIES TIMEOUT 180)
    add_test(NAME Bolt.ExtensionScope
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/extension_scope.py"
        --bolt "$<TARGET_FILE:Bolt>")
    set_tests_properties(Bolt.ExtensionScope PROPERTIES TIMEOUT 180)
    if(NOT BOLT_SANITIZE)
        add_test(NAME Bolt.IterationExtensions
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/iteration_extensions.py"
            --bolt "$<TARGET_FILE:Bolt>" --cxx "${CMAKE_CXX_COMPILER}")
        set_tests_properties(Bolt.IterationExtensions PROPERTIES TIMEOUT 300)
    endif()
endif()

# The frontend recursively checks expressions and instantiated generic bodies.
# Windows' default 1 MiB stack can overflow on ordinary mixed-argument methods
# before the language's explicit nesting guard is reached.
if(MSVC)
    foreach(frontend_host Bolt BoltTests BoltNativeTests BoltPreprocessorTests BoltEmbedding)
        if(TARGET ${frontend_host})
            target_link_options(${frontend_host} PRIVATE /STACK:8388608)
        endif()
    endforeach()
endif()

include(cmake/BoltToolchainOptimization.cmake)

if(APPLE AND BOLT_BUILD_TESTS AND Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_executable(BoltMachObjectProbe tests/MachObjectProbe.cpp)
    target_link_libraries(BoltMachObjectProbe PRIVATE BoltCore)
    add_test(NAME Bolt.MachObjectABI COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/macos_abi.py"
        --emitter "$<TARGET_FILE:BoltMachObjectProbe>")
endif()
if(APPLE AND BOLT_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_executable(BoltMacReloadProbe tests/macos_hot_reload.cpp)
    target_link_libraries(BoltMacReloadProbe PRIVATE BoltCore)
endif()
