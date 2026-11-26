cmake_minimum_required(VERSION 3.20)
project(ExternalFunkeHost LANGUAGES CXX)

set(BOLT_SDK_ROOT "$ENV{BOLT_SDK_ROOT}" CACHE PATH "Extracted or installed Bolt SDK directory")
if(NOT BOLT_SDK_ROOT)
    find_program(BOLT_HOST_LAUNCHER NAMES bolt bolt.exe bolt.cmd NO_CMAKE_FIND_ROOT_PATH)
    if(NOT BOLT_HOST_LAUNCHER OR CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Set -DBOLT_SDK_ROOT=<path to the SDK for the target>.")
    endif()
    get_filename_component(launcher "${BOLT_HOST_LAUNCHER}" REALPATH)
    get_filename_component(location "${launcher}" DIRECTORY)
    get_filename_component(BOLT_SDK_ROOT "${location}" DIRECTORY)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/BoltCompilerSDK.cmake")
bolt_import_compiler("${BOLT_SDK_ROOT}")

set(FUNKE_ENGINE_SOURCE_DIR "" CACHE PATH "Funke-Engine/src directory containing Scripting/NativeEngineAPI.hpp")
if(FUNKE_ENGINE_SOURCE_DIR)
    if(NOT EXISTS "${FUNKE_ENGINE_SOURCE_DIR}/Scripting/NativeEngineAPI.hpp")
        message(FATAL_ERROR "FUNKE_ENGINE_SOURCE_DIR must contain Scripting/NativeEngineAPI.hpp.")
    endif()
    set(funke_include "${FUNKE_ENGINE_SOURCE_DIR}")
else()
    # The simulator already vendors the original NativeEngineAPI.hpp at its recorded Funke commit.
    set(funke_include "${CMAKE_CURRENT_BINARY_DIR}/funke-include")
    configure_file("../FunkeEditorBench/Funke/NativeEngineAPI.hpp"
                   "${funke_include}/Scripting/NativeEngineAPI.hpp" COPYONLY)
endif()
add_executable(FunkeBoltHost "../FunkeBolt/headless.cpp")
target_compile_features(FunkeBoltHost PRIVATE cxx_std_20)
target_include_directories(FunkeBoltHost PRIVATE "${funke_include}")
bolt_link_compiler(FunkeBoltHost)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME Funke.HotReload COMMAND FunkeBoltHost
        "${CMAKE_CURRENT_LIST_DIR}/../FunkeBolt/Move.bolt"
        "${CMAKE_CURRENT_BINARY_DIR}/reloaded-script.bolt")
endif()
