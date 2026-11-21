# Shared by the source build and the installed SDK. All paths are supplied by
# the host: source tree or relocatable KromConfig.cmake.
function(krom_record_project_target target)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/target-paths/$<CONFIG>/${target}.txt"
        CONTENT "$<TARGET_FILE:${target}>")
endfunction()

function(krom_link_self_registering_addon target addon_target)
    target_link_libraries(${target} PRIVATE ${addon_target})
    if(MSVC)
        target_link_options(${target} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:${addon_target}>")
    elseif(APPLE)
        target_link_options(${target} PRIVATE "LINKER:-force_load,$<TARGET_FILE:${addon_target}>")
    else()
        target_link_options(${target} PRIVATE
            "LINKER:--whole-archive,$<TARGET_FILE:${addon_target}>,--no-whole-archive")
    endif()
endfunction()

function(krom_copy_assets target)
    if(NOT TARGET krom_copy_assets_to_bin)
        # Multi-config generators (Visual Studio) put executables in bin/<config>/.
        get_property(_krom_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(_krom_multi_config)
            set(_krom_bin_dest "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
        else()
            set(_krom_bin_dest "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
        endif()
        set(_krom_copy_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${KROM_ENGINE_RESOURCES_DIR}/shaders
                ${_krom_bin_dest}/shaders)
        if(EXISTS ${KROM_ENGINE_RESOURCES_DIR}/assets)
            list(APPEND _krom_copy_commands
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${KROM_ENGINE_RESOURCES_DIR}/assets
                    ${_krom_bin_dest}/assets)
        endif()
        if(Krom_SDK_ROOT AND EXISTS "${KROM_ENGINE_RESOURCES_DIR}/tools")
            list(APPEND _krom_copy_commands
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${KROM_ENGINE_RESOURCES_DIR}/tools"
                    "${_krom_bin_dest}/tools")
        endif()
        add_custom_target(krom_copy_assets_to_bin
            ${_krom_copy_commands}
            COMMENT "Copying shaders/ and assets/ to bin/")
    endif()
    add_dependencies(${target} krom_copy_assets_to_bin)
endfunction()

function(krom_copy_licenses target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${KROM_ENGINE_RESOURCES_DIR}/licenses"
            "$<TARGET_FILE_DIR:${target}>/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${KROM_LICENSE_FILE}"
            "$<TARGET_FILE_DIR:${target}>/licenses/KROM-LICENSE.txt")
endfunction()

function(krom_copy_d3dcompiler target)
    if(NOT WIN32)
        return()
    endif()

    # Only redistributable copies: the SDK's bundled one or the Windows SDK
    # redist folder. The System32 copy belongs to the OS and may not be shipped.
    set(_krom_programfiles_x86_var "ProgramFiles(x86)")
    set(_krom_d3dcompiler_candidates
        "${KROM_ENGINE_RESOURCES_DIR}/d3dcompiler_47.dll"
    )
    if(DEFINED ENV{WindowsSdkDir})
        list(APPEND _krom_d3dcompiler_candidates "$ENV{WindowsSdkDir}/Redist/D3D/x64/d3dcompiler_47.dll")
    endif()
    if(DEFINED ENV{${_krom_programfiles_x86_var}})
        list(APPEND _krom_d3dcompiler_candidates
            "$ENV{${_krom_programfiles_x86_var}}/Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll")
    endif()
    list(APPEND _krom_d3dcompiler_candidates
        "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll")
    foreach(_krom_d3dcompiler_path IN LISTS _krom_d3dcompiler_candidates)
        file(TO_CMAKE_PATH "${_krom_d3dcompiler_path}" _krom_d3dcompiler_path)
        if(EXISTS "${_krom_d3dcompiler_path}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_krom_d3dcompiler_path}"
                    "$<TARGET_FILE_DIR:${target}>/d3dcompiler_47.dll"
            )
            return()
        endif()
    endforeach()

    get_property(_krom_d3dcompiler_warned GLOBAL PROPERTY KROM_D3DCOMPILER_WARNED)
    if(NOT _krom_d3dcompiler_warned)
        set_property(GLOBAL PROPERTY KROM_D3DCOMPILER_WARNED TRUE)
        message(WARNING
            "d3dcompiler_47.dll (redistributable) not found, so it is not copied next to the "
            "executables. They still run on Windows 10/11 with the system copy, but builds meant "
            "for distribution should include it. Install the Windows SDK via the Visual Studio "
            "Installer (\"Windows 11 SDK\" or \"Windows 10 SDK\" under Individual components); "
            "the file is then found in Windows Kits/10/Redist/D3D/x64.")
    endif()
endfunction()

function(krom_apply_release_runtime_options target)
    if(NOT WIN32)
        return()
    endif()
    if(KROM_SHOW_CONSOLE)
        target_link_options(${target} PRIVATE
            "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS;/ENTRY:mainCRTStartup>")
    endif()
    if(KROM_MSVC_RUNTIME_DLLS)
        # One native CMake invocation avoids an oversized command that Ninja
        # turns into a batch file. cmd reads that UTF-8 file in the OEM codepage
        # and corrupts Unicode SDK/project paths.
        set(_copy_code "if(NOT [==[$<CONFIG>]==] STREQUAL Debug)\n  file(COPY\n")
        foreach(_krom_dll IN LISTS KROM_MSVC_RUNTIME_DLLS)
            string(APPEND _copy_code "    [==[${_krom_dll}]==]\n")
        endforeach()
        string(APPEND _copy_code "    DESTINATION [==[$<TARGET_FILE_DIR:${target}>]==])\nendif()\n")
        set(_copy_script "${CMAKE_CURRENT_BINARY_DIR}/runtime_files/${target}-$<CONFIG>.cmake")
        file(GENERATE OUTPUT "${_copy_script}" CONTENT "${_copy_code}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -P "${_copy_script}" VERBATIM)
    endif()
endfunction()

function(krom_link_project_scripts target)
    if(ARGC GREATER 1)
        set(_krom_project_root "${ARGV1}")
    else()
        krom_resolve_project_script_root(_krom_project_root)
    endif()
    if(NOT _krom_project_root)
        return()
    endif()

    # Entwicklungs-Default fuer Runtimes ohne Argument und ohne krom-project.json neben der EXE.
    target_compile_definitions(${target} PRIVATE
        KROM_DEFAULT_PROJECT_FILE="${_krom_project_root}/krom-project.json")

    file(GLOB_RECURSE _krom_project_script_sources CONFIGURE_DEPENDS
        "${_krom_project_root}/Assets/Scripts/*.cpp"
        "${_krom_project_root}/Assets/Scripts/*.cxx"
        "${_krom_project_root}/scripts/*.cpp"
        "${_krom_project_root}/scripts/*.cxx")
    file(GLOB_RECURSE _krom_project_script_headers CONFIGURE_DEPENDS
        "${_krom_project_root}/Assets/Scripts/*.hpp"
        "${_krom_project_root}/Assets/Scripts/*.h"
        "${_krom_project_root}/scripts/*.hpp"
        "${_krom_project_root}/scripts/*.h")

    set(_krom_project_bindings "${_krom_project_root}/generated/ScriptBindings.cpp")
    if(EXISTS "${_krom_project_bindings}")
        list(APPEND _krom_project_script_sources "${_krom_project_bindings}")
    endif()

    if(NOT _krom_project_script_sources AND NOT _krom_project_script_headers)
        return()
    endif()

    target_sources(${target} PRIVATE ${_krom_project_script_sources} ${_krom_project_script_headers})
    target_include_directories(${target} PRIVATE
        "${_krom_project_root}"
        "${_krom_project_root}/Assets/Scripts"
        "${_krom_project_root}/scripts"
        "${_krom_project_root}/generated")
    # KromRegisterGameScripts ist nur in den generierten Bindings definiert.
    if(EXISTS "${_krom_project_bindings}")
        target_compile_definitions(${target} PRIVATE KROM_HAS_GAME_SCRIPTS=1)
    else()
        message(WARNING "  ${target}: ${_krom_project_bindings} fehlt - Projekt-Scripts werden nicht registriert. "
                        "Im Editor die Script-Bindings neu generieren.")
    endif()
    message(STATUS "  ${target}: Projekt-Scripts eingebunden aus ${_krom_project_root}")
endfunction()

function(krom_add_project_script_module project_root)
    krom_read_project_runtime_target("${project_root}" _project)
    set(_target "krom_scripts_${_project}")
    file(GLOB_RECURSE _scripts CONFIGURE_DEPENDS
        "${project_root}/Assets/Scripts/*.cpp" "${project_root}/Assets/Scripts/*.cxx"
        "${project_root}/Assets/Scripts/*.h" "${project_root}/Assets/Scripts/*.hpp"
        "${project_root}/scripts/*.cpp" "${project_root}/scripts/*.cxx"
        "${project_root}/scripts/*.h" "${project_root}/scripts/*.hpp")
    file(GLOB _bindings CONFIGURE_DEPENDS "${project_root}/generated/ScriptBindings.cpp")
    set(_entry "${KROM_PROJECT_SUPPORT_ROOT}/addons/script/ScriptModuleEntry.cpp")
    if(Krom_SDK_ROOT)
        # Keep CMake's object names short in deeply nested project-local caches.
        # An entrypoint outside this source tree otherwise adds a hash directory.
        configure_file("${_entry}" "${CMAKE_CURRENT_BINARY_DIR}/krom_sdk_module.cpp" COPYONLY)
        set(_entry "${CMAKE_CURRENT_BINARY_DIR}/krom_sdk_module.cpp")
    endif()
    add_library(${_target} SHARED EXCLUDE_FROM_ALL ${_scripts} ${_bindings} "${_entry}")
    krom_enable_unicode_link(${_target})
    target_include_directories(${_target} PRIVATE "${project_root}"
        "${project_root}/Assets/Scripts" "${project_root}/scripts" "${project_root}/generated")
    target_link_libraries(${_target} PRIVATE ${KROM_TARGET_NAMESPACE}engine_script)
    target_compile_definitions(${_target} PRIVATE KROM_SCRIPT_PROJECT_ROOT="${project_root}")
    set_target_properties(${_target} PROPERTIES PREFIX "")
    if(_bindings)
        target_compile_definitions(${_target} PRIVATE KROM_HAS_GAME_SCRIPTS=1)
    endif()
    if(MSVC)
        target_link_options(${_target} PRIVATE /EXPORT:KromRegisterGameScripts)
        # Auch die Link-Ausgabe braucht je Build einen eigenen PDB-Namen:
        # Debugger koennen trotz Schattenkopie die originale PDB offen halten.
        set(_pdb_response "${CMAKE_CURRENT_BINARY_DIR}/script_pdb/${_target}.rsp")
        set(_pdb_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WriteScriptModulePdb.cmake")
        add_custom_command(TARGET ${_target} PRE_LINK
            COMMAND "${CMAKE_COMMAND}"
                "-DKROM_SCRIPT_PDB_DIR=$<TARGET_FILE_DIR:${_target}>"
                "-DKROM_SCRIPT_PDB_TARGET=${_target}"
                "-DKROM_SCRIPT_PDB_RESPONSE=${_pdb_response}"
                -P "${_pdb_script}"
            VERBATIM)
        set_property(TARGET ${_target} APPEND PROPERTY LINK_DEPENDS "${_pdb_script}")
        target_link_options(${_target} PRIVATE /INCREMENTAL:NO "@${_pdb_response}" /PDBALTPATH:%_PDB%)
    endif()
    krom_record_project_target(${_target})
endfunction()

function(krom_add_project_runtime target output_name platform_target backend_addon backend_define project_root)
    set(_entry "${KROM_PROJECT_SUPPORT_ROOT}/examples/editor_scene_runtime.cpp")
    if(Krom_SDK_ROOT)
        configure_file("${_entry}" "${CMAKE_CURRENT_BINARY_DIR}/krom_sdk_runtime.cpp" COPYONLY)
        set(_entry "${CMAKE_CURRENT_BINARY_DIR}/krom_sdk_runtime.cpp")
    endif()
    add_executable(${target} "${_entry}")
    krom_enable_unicode_link(${target})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output_name}")
    krom_apply_console_option(${target})

    if("${platform_target}" MATCHES "(^|::)engine_platform_win32$")
        target_link_libraries(${target} PRIVATE ${KROM_TARGET_NAMESPACE}engine_krom_wrapper_win32)
    elseif("${platform_target}" MATCHES "(^|::)engine_platform_glfw$")
        target_link_libraries(${target} PRIVATE ${KROM_TARGET_NAMESPACE}engine_krom_wrapper_glfw)
    endif()

    krom_link_self_registering_addon(${target} ${backend_addon})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE ${backend_define}=1)
    krom_link_project_scripts(${target} "${project_root}")
    krom_apply_release_runtime_options(${target})

    krom_copy_assets(${target})
    krom_copy_licenses(${target})
    if("${backend_define}" STREQUAL "KROM_APP_BACKEND_DX11")
        krom_copy_d3dcompiler(${target})
    endif()
    krom_record_project_target(${target})
endfunction()
