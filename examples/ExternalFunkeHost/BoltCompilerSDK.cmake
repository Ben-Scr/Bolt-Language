# Import the public C ABI from a prebuilt desktop SDK.
function(bolt_import_compiler sdk_root)
    if(TARGET Bolt::Compiler)
        return()
    endif()
    get_filename_component(root "${sdk_root}" ABSOLUTE)
    if(WIN32)
        set(library_name BoltCompiler.dll)
    elseif(APPLE)
        set(library_name libBoltCompiler.dylib)
    elseif(CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
        set(library_name libBoltCompiler.so)
    else()
        message(FATAL_ERROR "No desktop Bolt SDK layout for ${CMAKE_SYSTEM_NAME}")
    endif()

    # The packaged SDK keeps its C headers together with its compiler library.
    # An explicit root also selects the target architecture in cross builds.
    set(compiler_directory "")
    foreach(directory IN ITEMS "${root}/bin" "${root}/libexec" "${root}")
        if(EXISTS "${directory}/${library_name}" AND EXISTS "${directory}/include/bolt/CompilerHost.h")
            set(compiler_directory "${directory}")
            break()
        endif()
    endforeach()
    if(NOT compiler_directory)
        message(FATAL_ERROR "Expected ${library_name} and include/bolt/CompilerHost.h in the Bolt SDK at '${root}'.")
    endif()
    add_library(Bolt::Compiler SHARED IMPORTED GLOBAL)
    set_target_properties(Bolt::Compiler PROPERTIES
        IMPORTED_LOCATION "${compiler_directory}/${library_name}"
        INTERFACE_INCLUDE_DIRECTORIES "${compiler_directory}/include")
    if(WIN32)
        if(NOT EXISTS "${compiler_directory}/BoltCompiler.lib")
            message(FATAL_ERROR "The Windows SDK at '${compiler_directory}' is missing BoltCompiler.lib.")
        endif()
        set_target_properties(Bolt::Compiler PROPERTIES IMPORTED_IMPLIB "${compiler_directory}/BoltCompiler.lib")
        if(EXISTS "${compiler_directory}/LLVM-C.dll")
            set_target_properties(Bolt::Compiler PROPERTIES BOLT_LLVM_RUNTIME "${compiler_directory}/LLVM-C.dll")
        endif()
    endif()
endfunction()

function(bolt_link_compiler executable_target)
    target_link_libraries("${executable_target}" PRIVATE Bolt::Compiler)
    if(APPLE OR WIN32)
        add_custom_command(TARGET "${executable_target}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:Bolt::Compiler>" "$<TARGET_FILE_DIR:${executable_target}>"
            VERBATIM)
    endif()
    if(APPLE)
        # This LC_RPATH remains valid when the editor and its sibling dylib move together.
        target_link_options("${executable_target}" PRIVATE "LINKER:-rpath,@executable_path")
    elseif(WIN32)
        get_target_property(llvm Bolt::Compiler BOLT_LLVM_RUNTIME)
        if(llvm)
            add_custom_command(TARGET "${executable_target}" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${llvm}" "$<TARGET_FILE_DIR:${executable_target}>"
                VERBATIM)
        endif()
    else()
        # Explicit for existing engine targets that already have their own build RPATH.
        set_property(TARGET "${executable_target}" APPEND PROPERTY BUILD_RPATH "$<TARGET_FILE_DIR:Bolt::Compiler>")
    endif()
endfunction()
