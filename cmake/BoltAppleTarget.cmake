# Resolve the deployment target BEFORE enabling the Apple toolchain. CMake 4+
# otherwise leaves this empty with Make/Ninja, so the SDK may select a newer
# target for the runtime than the target used by our own object/linker pipeline.
if(CMAKE_HOST_APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        if(DEFINED ENV{MACOSX_DEPLOYMENT_TARGET} AND NOT "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
            set(_bolt_macos_target "$ENV{MACOSX_DEPLOYMENT_TARGET}")
        elseif(CMAKE_HOST_APPLE)
            execute_process(COMMAND /usr/bin/sw_vers -productVersion
                OUTPUT_VARIABLE _bolt_macos_target OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _bolt_sw_vers_status)
            if(NOT _bolt_sw_vers_status EQUAL 0)
                message(FATAL_ERROR "Cannot determine macOS version; set -DCMAKE_OSX_DEPLOYMENT_TARGET explicitly")
            endif()
        else()
            message(FATAL_ERROR "Cross-building for macOS requires -DCMAKE_OSX_DEPLOYMENT_TARGET and an SDK")
        endif()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${_bolt_macos_target}" CACHE STRING "Minimum supported macOS" FORCE)
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?$")
        message(FATAL_ERROR "CMAKE_OSX_DEPLOYMENT_TARGET must be a numeric macOS version")
    endif()
endif()
