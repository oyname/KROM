cmake_minimum_required(VERSION 3.22)
project(KROMEngine VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP /wd4100 /wd4201)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    else()
        add_compile_options(-O2 -DNDEBUG)
    endif()
endif()

option(KROM_USE_GLFW "Build GLFW platform layer when GLFW is available" ON)
option(KROM_BUILD_DX11 "Build DX11 backend" ${WIN32})
if(WIN32 OR UNIX OR APPLE)
    option(KROM_BUILD_OPENGL "Build OpenGL backend" ON)
else()
    option(KROM_BUILD_OPENGL "Build OpenGL backend" OFF)
endif()
option(KROM_BUILD_VULKAN "Build Vulkan backend" ON)
option(KROM_BUILD_EDITOR "Build in-app ImGui editor" ON)

find_package(Threads REQUIRED)

add_library(engine_base INTERFACE)
target_include_directories(engine_base
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(engine_base INTERFACE Threads::Threads)
target_compile_definitions(engine_base INTERFACE KROM_HAS_GLFW=0)

add_library(engine_renderer_api INTERFACE)
target_include_directories(engine_renderer_api
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(engine_renderer_api INTERFACE engine_base)

add_library(engine_rendergraph_layer STATIC
    src/rendergraph/RenderGraph.cpp
    src/rendergraph/ResourceAliaser.cpp)
target_include_directories(engine_rendergraph_layer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(engine_rendergraph_layer PUBLIC engine_renderer_api)

add_library(engine_material_layer STATIC
    src/renderer/ShaderParameterLayout.cpp
    src/renderer/ParameterBlob.cpp
    src/renderer/MaterialSystem.cpp
    src/renderer/MaterialFeatureEval.cpp
    src/renderer/MaterialCBLayout.cpp
    src/renderer/VertexLayoutContract.cpp)
target_include_directories(engine_material_layer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(engine_material_layer PUBLIC engine_renderer_api)

add_library(engine_shader_layer STATIC
    src/renderer/ShaderCompiler.cpp
    src/renderer/ShaderCompilerShared.cpp
    src/renderer/ShaderCompilerD3D.cpp
    src/renderer/ShaderCompilerDXC.cpp
    src/renderer/ShaderCompilerSPIRV.cpp
    src/renderer/ShaderCompilerGLSL.cpp
    src/renderer/ShaderRuntime.cpp
    src/renderer/ShaderRuntimeResources.cpp
    src/renderer/ShaderRuntimeAssets.cpp
    src/renderer/ShaderRuntimeMaterials.cpp)
target_include_directories(engine_shader_layer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer
)
target_link_libraries(engine_shader_layer PUBLIC engine_renderer_api engine_material_layer)

add_library(engine_asset_pipeline STATIC
    src/assets/AssetPipeline.cpp
    src/assets/KMeshSerializer.cpp
    src/assets/ImageDecoder.cpp
    src/assets/TextureImporter.cpp
    src/assets/MeshTangents.cpp)
target_include_directories(engine_asset_pipeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(engine_asset_pipeline PUBLIC engine_base engine_renderer_api engine_material_layer engine_shader_layer)

add_library(engine_renderer_runtime_layer STATIC
    src/renderer/RenderExtractionContext.cpp
    src/renderer/RenderPassRegistry.cpp
    src/renderer/RenderSceneSnapshot.cpp
    src/renderer/RenderWorld.cpp
    src/renderer/GpuResourceRuntime.cpp
    src/renderer/PipelineCache.cpp)
target_include_directories(engine_renderer_runtime_layer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(engine_renderer_runtime_layer PUBLIC
    engine_renderer_api
    engine_rendergraph_layer
    engine_material_layer
    engine_asset_pipeline
)

add_library(engine_renderer_backend_layer STATIC
    src/renderer/DeviceFactory.cpp)
target_include_directories(engine_renderer_backend_layer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(engine_renderer_backend_layer PUBLIC engine_renderer_api)

add_library(engine_environment_layer STATIC
    src/renderer/EnvironmentSystem.cpp
    src/renderer/IBLBaker.cpp
    src/renderer/IBLCacheSerializer.cpp
    src/renderer/IBLResourceLoader.cpp)
target_include_directories(engine_environment_layer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer
)
target_link_libraries(engine_environment_layer PUBLIC engine_renderer_api engine_asset_pipeline engine_shader_layer engine_renderer_runtime_layer)

add_library(engine_core STATIC
    # Core
    src/core/Logger.cpp
    src/core/InternedString.cpp
    src/core/ModuleSystem.cpp
    # ECS
    src/ecs/Archetype.cpp
    src/ecs/World.cpp
    # Scene
    src/scene/Scene.cpp
    src/scene/TransformSystem.cpp
    src/scene/BoundsSystem.cpp
    # Jobs
    src/jobs/JobSystem.cpp
    # Serialization
    src/serialization/SceneSerializer.cpp
    # Collision
    src/collision/SceneQueries.cpp
    # Platform
    src/platform/Window.cpp
    src/platform/StdFilesystem.cpp
    # Nativer Datei-Dialog: Win32-Impl auf Windows, Stub (gibt "" zurueck) auf Linux/macOS.
    # Fuer Linux: NativeFileDialog_Stub.cpp durch eine GTK- oder zenity-Implementierung ersetzen.
    # Fuer macOS: NativeFileDialog_Stub.cpp durch eine NSOpenPanel-Implementierung (.mm) ersetzen.
    $<IF:$<BOOL:${WIN32}>,src/platform/NativeFileDialog_Win32.cpp,src/platform/NativeFileDialog_Stub.cpp>)

target_include_directories(engine_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer  # internal headers (FloatConvert.hpp, …)
)

target_link_libraries(engine_core PUBLIC Threads::Threads)
if(WIN32)
    # Ole32: benoetigt fuer CoInitializeEx / CoCreateInstance (IFileOpenDialog)
    target_link_libraries(engine_core PRIVATE Ole32)
endif()
target_compile_definitions(engine_core PUBLIC KROM_HAS_GLFW=0)

add_library(engine_renderer_orchestration_layer STATIC
    src/renderer/ECSExtractor.cpp
    src/renderer/FeatureRegistry.cpp
    src/renderer/FrameExtractionStage.cpp
    src/renderer/FrameConstantStage.cpp
    src/renderer/FrameShaderStage.cpp
    src/renderer/FrameUploadStage.cpp
    src/renderer/FrameGraphStage.cpp
    src/renderer/FrameExecutionStage.cpp
    src/renderer/RenderFrameOrchestrator.cpp
    src/renderer/RenderSystem.cpp
    src/renderer/PlatformRenderLoop.cpp)
target_include_directories(engine_renderer_orchestration_layer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer
)
target_link_libraries(engine_renderer_orchestration_layer PUBLIC
    engine_core
    engine_rendergraph_layer
    engine_material_layer
    engine_shader_layer
    engine_asset_pipeline
    engine_renderer_backend_layer
    engine_environment_layer
    engine_renderer_runtime_layer
)

set(KROM_HAS_SHADERC 0)
set(KROM_SHADERC_TARGET "")

find_package(shaderc CONFIG QUIET)
if(NOT shaderc_FOUND)
    find_package(unofficial-shaderc CONFIG QUIET)
endif()

foreach(_krom_shaderc_target
        shaderc::shaderc
        shaderc_combined
        shaderc_shared
        shaderc)
    if(TARGET ${_krom_shaderc_target})
        set(KROM_SHADERC_TARGET ${_krom_shaderc_target})
        break()
    endif()
endforeach()

if(KROM_SHADERC_TARGET)
    target_link_libraries(engine_shader_layer PRIVATE ${KROM_SHADERC_TARGET})
    set(KROM_HAS_SHADERC 1)
    message(STATUS "  shaderc: ENABLED via CMake target '${KROM_SHADERC_TARGET}'")
else()
    set(_krom_shaderc_hints)
    if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
        list(APPEND _krom_shaderc_hints
            "$ENV{VULKAN_SDK}"
            "$ENV{VULKAN_SDK}/Include"
            "$ENV{VULKAN_SDK}/Lib")
    endif()

    find_path(SHADERC_INCLUDE_DIR
        NAMES shaderc/shaderc.hpp
        HINTS ${_krom_shaderc_hints}
        PATH_SUFFIXES Include include)

    find_library(SHADERC_LIBRARY
        NAMES shaderc_combined shaderc_shared shaderc
        HINTS ${_krom_shaderc_hints}
        PATH_SUFFIXES Lib lib)

    if(SHADERC_INCLUDE_DIR AND SHADERC_LIBRARY)
        message(STATUS "  shaderc: headers/library found, but manual library linking is disabled to avoid MSVC Debug/Release STL mismatches; using tool fallback instead")
    else()
        message(STATUS "  shaderc: disabled (shaderc headers/library not found)")
    endif()
endif()

target_compile_definitions(engine_shader_layer PUBLIC KROM_HAS_SHADERC=${KROM_HAS_SHADERC})

if(KROM_USE_GLFW)
    find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h)
    find_library(GLFW_LIBRARY NAMES glfw glfw3)
    if(GLFW_INCLUDE_DIR AND GLFW_LIBRARY)
        add_library(engine_platform_glfw STATIC
            src/platform/glfw/GLFWPlatform.cpp
            src/platform/glfw/GLFWWindow.cpp
            src/platform/glfw/GLFWInput.cpp
            src/platform/glfw/GLFWThread.cpp
        )
        target_include_directories(engine_platform_glfw
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
            PRIVATE
                ${GLFW_INCLUDE_DIR}
        )
        target_link_libraries(engine_platform_glfw PUBLIC engine_core Threads::Threads ${GLFW_LIBRARY})
        if(UNIX AND NOT APPLE)
            find_package(X11 QUIET)
            if(X11_FOUND)
                target_link_libraries(engine_platform_glfw PRIVATE X11::X11)
            endif()
        endif()
        target_compile_definitions(engine_platform_glfw PUBLIC KROM_HAS_GLFW=1)
        set_property(TARGET engine_core APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS KROM_HAS_GLFW=1)
        message(STATUS "  GLFW platform: ENABLED")
    else()
        message(STATUS "  GLFW platform: disabled (GLFW headers/library not found)")
    endif()
endif()

if(WIN32)
    add_library(engine_platform_win32 STATIC
        src/platform/win32/Win32Platform.cpp
        src/platform/win32/Win32Window.cpp
        src/platform/win32/Win32Input.cpp
        src/platform/win32/Win32Thread.cpp)
    target_include_directories(engine_platform_win32
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32>
    )
    target_link_libraries(engine_platform_win32 PUBLIC engine_core)
    message(STATUS "  Win32 platform: ENABLED")
else()
    message(STATUS "  Win32 platform: disabled (not Windows)")
endif()

# ── AddOns ────────────────────────────────────────────────────
add_subdirectory(addons/camera)
add_subdirectory(addons/lighting)
add_subdirectory(addons/mesh_renderer)
add_subdirectory(addons/particles)
add_subdirectory(addons/null)
add_subdirectory(addons/dx11)
add_subdirectory(addons/opengl)
add_subdirectory(addons/vulkan)
add_subdirectory(addons/forward)
add_subdirectory(addons/debug_draw)
add_subdirectory(addons/pbr)
add_subdirectory(addons/unlit)
add_subdirectory(addons/lit)
add_subdirectory(addons/shadow)
add_subdirectory(addons/gtao)
add_subdirectory(addons/outline)
add_subdirectory(addons/animation)
add_subdirectory(addons/script)
add_subdirectory(addons/prefab)
add_subdirectory(addons/gltf)
if(KROM_BUILD_EDITOR)
    add_subdirectory(addons/editor)
endif()

if(TARGET engine_platform_win32)
    add_library(engine_krom_wrapper_win32 STATIC
        src/app/KromWrapper.cpp)
    target_include_directories(engine_krom_wrapper_win32
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_definitions(engine_krom_wrapper_win32 PUBLIC KROM_APP_USE_WIN32_PLATFORM=1)
    target_link_libraries(engine_krom_wrapper_win32
        PUBLIC
            engine_renderer_orchestration_layer
            engine_camera
            engine_forward
            engine_lighting
            engine_lit
            engine_unlit
            engine_pbr
            engine_mesh_renderer
            engine_shadow
            engine_gtao
            engine_animation
            engine_prefab
    )
    target_link_libraries(engine_krom_wrapper_win32 PUBLIC engine_platform_win32 engine_script)
    if(TARGET engine_gltf)
        target_link_libraries(engine_krom_wrapper_win32 PUBLIC engine_gltf)
        target_compile_definitions(engine_krom_wrapper_win32 PUBLIC KROM_WRAPPER_HAS_GLTF=1)
    endif()
endif()

if(TARGET engine_platform_glfw)
    add_library(engine_krom_wrapper_glfw STATIC
        src/app/KromWrapper.cpp)
    target_include_directories(engine_krom_wrapper_glfw
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_definitions(engine_krom_wrapper_glfw PUBLIC KROM_APP_USE_GLFW_PLATFORM=1)
    target_link_libraries(engine_krom_wrapper_glfw
        PUBLIC
            engine_renderer_orchestration_layer
            engine_camera
            engine_forward
            engine_lighting
            engine_lit
            engine_unlit
            engine_pbr
            engine_mesh_renderer
            engine_shadow
            engine_gtao
            engine_animation
            engine_prefab
            engine_platform_glfw
            engine_script
    )
    if(TARGET engine_gltf)
        target_link_libraries(engine_krom_wrapper_glfw PUBLIC engine_gltf)
        target_compile_definitions(engine_krom_wrapper_glfw PUBLIC KROM_WRAPPER_HAS_GLTF=1)
    endif()
endif()

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
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets
                $<TARGET_FILE_DIR:${target}>/assets
        )
    endif()
endfunction()

function(krom_copy_d3dcompiler target)
    if(NOT WIN32)
        return()
    endif()

    set(_krom_d3dcompiler_candidates
        "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll"
        "$ENV{WINDIR}/System32/D3DCompiler_47.dll"
    )
    foreach(_krom_d3dcompiler_path IN LISTS _krom_d3dcompiler_candidates)
        if(EXISTS "${_krom_d3dcompiler_path}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_krom_d3dcompiler_path}"
                    "$<TARGET_FILE_DIR:${target}>/d3dcompiler_47.dll"
            )
            return()
        endif()
    endforeach()
endfunction()

set(KROM_PROJECT_SCRIPT_ROOT "" CACHE PATH "Optional KROM project root whose C++ scripts are linked into editor targets")

function(krom_resolve_project_script_root out_var)
    if(KROM_PROJECT_SCRIPT_ROOT)
        set(${out_var} "${KROM_PROJECT_SCRIPT_ROOT}" PARENT_SCOPE)
        return()
    endif()

    file(GLOB _krom_project_files CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/projects/*/krom-project.json")
    list(LENGTH _krom_project_files _krom_project_count)
    if(_krom_project_count EQUAL 1)
        list(GET _krom_project_files 0 _krom_project_file)
        get_filename_component(_krom_project_root "${_krom_project_file}" DIRECTORY)
        set(${out_var} "${_krom_project_root}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
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
    target_compile_definitions(${target} PRIVATE KROM_HAS_GAME_SCRIPTS=1)
    message(STATUS "  ${target}: Projekt-Scripts eingebunden aus ${_krom_project_root}")
endfunction()

function(krom_sanitize_identifier input out_var)
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _sanitized "${input}")
    if(_sanitized STREQUAL "")
        set(_sanitized "KromGame")
    endif()
    if(_sanitized MATCHES "^[0-9]")
        set(_sanitized "Krom_${_sanitized}")
    endif()
    set(${out_var} "${_sanitized}" PARENT_SCOPE)
endfunction()

function(krom_read_project_runtime_target project_root out_var)
    set(_runtime_target "")
    set(_project_file "${project_root}/krom-project.json")
    if(EXISTS "${_project_file}")
        file(READ "${_project_file}" _project_json)
        string(REGEX MATCH "\"runtimeTarget\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" _runtime_match "${_project_json}")
        if(_runtime_match)
            set(_runtime_target "${CMAKE_MATCH_1}")
        else()
            string(REGEX MATCH "\"name\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" _name_match "${_project_json}")
            if(_name_match)
                set(_runtime_target "${CMAKE_MATCH_1}")
            endif()
        endif()
    endif()
    if(_runtime_target STREQUAL "")
        get_filename_component(_runtime_target "${project_root}" NAME)
    endif()
    krom_sanitize_identifier("${_runtime_target}" _runtime_target)
    set(${out_var} "${_runtime_target}" PARENT_SCOPE)
endfunction()

function(krom_add_project_runtime target output_name platform_target backend_addon backend_define project_root)
    add_executable(${target} examples/editor_scene_runtime.cpp)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output_name}")

    if(${platform_target} STREQUAL "engine_platform_win32")
        target_link_libraries(${target} PRIVATE engine_krom_wrapper_win32)
    elseif(${platform_target} STREQUAL "engine_platform_glfw")
        target_link_libraries(${target} PRIVATE engine_krom_wrapper_glfw)
    endif()

    krom_link_self_registering_addon(${target} ${backend_addon})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE ${backend_define}=1)
    krom_link_project_scripts(${target} "${project_root}")

    krom_copy_assets(${target})
    if("${backend_define}" STREQUAL "KROM_APP_BACKEND_DX11")
        krom_copy_d3dcompiler(${target})
    endif()
endfunction()

function(krom_add_project_runtimes_for_backend platform_target backend_addon backend_define backend_id)
    file(GLOB _krom_project_files CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/projects/*/krom-project.json")
    foreach(_krom_project_file IN LISTS _krom_project_files)
        get_filename_component(_krom_project_root "${_krom_project_file}" DIRECTORY)
        krom_read_project_runtime_target("${_krom_project_root}" _runtime_target)
        set(_target "krom_runtime_${_runtime_target}_${backend_id}")
        set(_output "${_runtime_target}_${backend_id}")
        if(NOT TARGET ${_target})
            krom_add_project_runtime(${_target} ${_output}
                ${platform_target} ${backend_addon} ${backend_define} "${_krom_project_root}")
        endif()
    endforeach()
endfunction()

function(krom_add_editor target platform_target backend_addon backend_define)
    add_executable(${target}
        app/KromEditorApp.cpp
        app/editor/editor_main.cpp
        app/editor/EditorScene.cpp
        app/editor/EditorWin32Hook.cpp)
    target_link_libraries(${target} PRIVATE
        engine_renderer_orchestration_layer
        engine_camera
        engine_forward
        engine_debug_draw
        engine_lighting
        engine_mesh_renderer
        engine_pbr
        engine_unlit
        engine_lit
        engine_shadow
        engine_gtao
        engine_outline
        engine_animation
        engine_prefab
        engine_editor
        engine_script
        ${platform_target}
    )
    if(TARGET engine_gltf)
        target_link_libraries(${target} PRIVATE engine_gltf)
    endif()
    krom_link_self_registering_addon(${target} ${backend_addon})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/forward
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/debug_draw
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/pbr
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/gtao
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/outline
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/animation
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/prefab
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/lit
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/gltf
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/dx11
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/editor
    )
    target_compile_definitions(${target} PRIVATE ${backend_define}=1)
    target_compile_definitions(${target} PRIVATE
        KROM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        KROM_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}")

    if(${platform_target} STREQUAL "engine_platform_win32")
        target_compile_definitions(${target} PRIVATE KROM_APP_USE_WIN32_PLATFORM=1)
    elseif(${platform_target} STREQUAL "engine_platform_glfw")
        target_compile_definitions(${target} PRIVATE KROM_APP_USE_GLFW_PLATFORM=1)
    endif()

    krom_link_project_scripts(${target})

    krom_copy_assets(${target})
    if("${backend_define}" STREQUAL "KROM_APP_BACKEND_DX11")
        krom_copy_d3dcompiler(${target})
    endif()
endfunction()

function(krom_add_wrapper_example target platform_target backend_addon backend_define)
    krom_add_wrapper_example_src(${target} ${platform_target} ${backend_addon} ${backend_define} examples/load_3d_model.cpp)
endfunction()

function(krom_add_wrapper_example_src target platform_target backend_addon backend_define src)
    add_executable(${target} ${src})
    if(${platform_target} STREQUAL "engine_platform_win32")
        target_link_libraries(${target} PRIVATE engine_krom_wrapper_win32)
    elseif(${platform_target} STREQUAL "engine_platform_glfw")
        target_link_libraries(${target} PRIVATE engine_krom_wrapper_glfw)
    endif()
    if(TARGET engine_dx11)
        krom_link_self_registering_addon(${target} engine_dx11)
    endif()
    if(TARGET engine_vulkan)
        krom_link_self_registering_addon(${target} engine_vulkan)
    endif()
    if(TARGET engine_opengl)
        krom_link_self_registering_addon(${target} engine_opengl)
    endif()
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_definitions(${target} PRIVATE ${backend_define}=1)
    krom_link_project_scripts(${target})

    krom_copy_assets(${target})
    if("${backend_define}" STREQUAL "KROM_APP_BACKEND_DX11")
        krom_copy_d3dcompiler(${target})
    endif()
endfunction()

if(KROM_BUILD_DX11 AND TARGET engine_dx11 AND TARGET engine_platform_win32)
    if(KROM_BUILD_EDITOR AND TARGET engine_editor)
        krom_add_editor(krom_editor_dx11 engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11)
    endif()
    krom_add_wrapper_example(load_3d_model_dx11 engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11)
    krom_add_wrapper_example_src(adapter_enum_dx11    engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11 examples/adapter_enum.cpp)
    krom_add_wrapper_example_src(minimal_window_dx11  engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11 examples/minimal_window.cpp)
    krom_add_wrapper_example_src(textured_quad_dx11   engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11 examples/textured_quad.cpp)
    krom_add_wrapper_example_src(textured_cube_dx11   engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11 examples/textured_cube.cpp)
    krom_add_wrapper_example_src(ibl_sphere_dx11      engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11 examples/ibl_sphere.cpp)
    krom_add_wrapper_example_src(editor_scene_runtime_dx11 engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11 examples/editor_scene_runtime.cpp)
    krom_add_project_runtimes_for_backend(engine_platform_win32 engine_dx11 KROM_APP_BACKEND_DX11 dx11)
endif()

if(KROM_BUILD_VULKAN AND TARGET engine_vulkan)
    if(TARGET engine_platform_win32)
        if(KROM_BUILD_EDITOR AND TARGET engine_editor)
            krom_add_editor(krom_editor_vulkan engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN)
        endif()
        krom_add_wrapper_example(load_3d_model_vulkan engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN)
        krom_add_wrapper_example_src(adapter_enum_vulkan   engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN examples/adapter_enum.cpp)
        krom_add_wrapper_example_src(minimal_window_vulkan engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN examples/minimal_window.cpp)
        krom_add_wrapper_example_src(textured_quad_vulkan  engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN examples/textured_quad.cpp)
        krom_add_wrapper_example_src(textured_cube_vulkan  engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN examples/textured_cube.cpp)
        krom_add_wrapper_example_src(ibl_sphere_vulkan     engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN examples/ibl_sphere.cpp)
        krom_add_wrapper_example_src(editor_scene_runtime_vulkan engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN examples/editor_scene_runtime.cpp)
        krom_add_project_runtimes_for_backend(engine_platform_win32 engine_vulkan KROM_APP_BACKEND_VULKAN vulkan)
    elseif(TARGET engine_platform_glfw)
        if(KROM_BUILD_EDITOR AND TARGET engine_editor)
            krom_add_editor(krom_editor_vulkan engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN)
        endif()
        krom_add_wrapper_example(load_3d_model_vulkan engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN)
        krom_add_wrapper_example_src(adapter_enum_vulkan   engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN examples/adapter_enum.cpp)
        krom_add_wrapper_example_src(minimal_window_vulkan engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN examples/minimal_window.cpp)
        krom_add_wrapper_example_src(textured_quad_vulkan  engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN examples/textured_quad.cpp)
        krom_add_wrapper_example_src(textured_cube_vulkan  engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN examples/textured_cube.cpp)
        krom_add_wrapper_example_src(ibl_sphere_vulkan     engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN examples/ibl_sphere.cpp)
        krom_add_wrapper_example_src(editor_scene_runtime_vulkan engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN examples/editor_scene_runtime.cpp)
        krom_add_project_runtimes_for_backend(engine_platform_glfw engine_vulkan KROM_APP_BACKEND_VULKAN vulkan)
    endif()
endif()

if(KROM_BUILD_OPENGL AND TARGET engine_opengl)
    if(TARGET engine_platform_win32)
        if(KROM_BUILD_EDITOR AND TARGET engine_editor)
            krom_add_editor(krom_editor_opengl engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL)
        endif()
        krom_add_wrapper_example(load_3d_model_opengl engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL)
        krom_add_wrapper_example_src(adapter_enum_opengl   engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL examples/adapter_enum.cpp)
        krom_add_wrapper_example_src(minimal_window_opengl engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL examples/minimal_window.cpp)
        krom_add_wrapper_example_src(textured_quad_opengl  engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL examples/textured_quad.cpp)
        krom_add_wrapper_example_src(textured_cube_opengl  engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL examples/textured_cube.cpp)
        krom_add_wrapper_example_src(ibl_sphere_opengl     engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL examples/ibl_sphere.cpp)
        krom_add_wrapper_example_src(editor_scene_runtime_opengl engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL examples/editor_scene_runtime.cpp)
        krom_add_project_runtimes_for_backend(engine_platform_win32 engine_opengl KROM_APP_BACKEND_OPENGL opengl)
    elseif(TARGET engine_platform_glfw)
        if(KROM_BUILD_EDITOR AND TARGET engine_editor)
            krom_add_editor(krom_editor_opengl engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL)
        endif()
        krom_add_wrapper_example(load_3d_model_opengl engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL)
        krom_add_wrapper_example_src(adapter_enum_opengl   engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL examples/adapter_enum.cpp)
        krom_add_wrapper_example_src(minimal_window_opengl engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL examples/minimal_window.cpp)
        krom_add_wrapper_example_src(textured_quad_opengl  engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL examples/textured_quad.cpp)
        krom_add_wrapper_example_src(textured_cube_opengl  engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL examples/textured_cube.cpp)
        krom_add_wrapper_example_src(ibl_sphere_opengl     engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL examples/ibl_sphere.cpp)
        krom_add_wrapper_example_src(editor_scene_runtime_opengl engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL examples/editor_scene_runtime.cpp)
        krom_add_project_runtimes_for_backend(engine_platform_glfw engine_opengl KROM_APP_BACKEND_OPENGL opengl)
    endif()
endif()

message(STATUS "===================================================")
message(STATUS "  KROM Engine v${PROJECT_VERSION}")
message(STATUS "  Build: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "===================================================")

# =============================================================================
# Tests
# =============================================================================
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp)
add_executable(krom_tests
    tests/main.cpp
    tests/test_ecs.cpp
    tests/test_jobs.cpp
    tests/test_renderer.cpp
    tests/test_rendergraph.cpp
    tests/test_platform.cpp
    tests/test_serialization.cpp
    tests/test_assets_collision.cpp
    tests/test_events.cpp
    tests/test_module_system.cpp
    tests/test_animation.cpp
    tests/test_prefab.cpp
    tests/test_gltf_importer.cpp
    tests/test_script.cpp)
target_include_directories(krom_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/forward
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/null
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/pbr
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/unlit
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/lit
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/gltf
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/animation
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/prefab
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/script
)

target_link_libraries(krom_tests PRIVATE
    engine_renderer_orchestration_layer
    engine_camera
    engine_forward
    engine_lighting
    engine_mesh_renderer
    engine_pbr
    engine_unlit
    engine_lit
    engine_shadow
    engine_animation
    engine_prefab
    engine_script
)
if(TARGET engine_gltf)
    target_link_libraries(krom_tests PRIVATE engine_gltf)
endif()
krom_link_self_registering_addon(krom_tests engine_null)
krom_link_self_registering_addon(krom_tests engine_dx11)
krom_link_self_registering_addon(krom_tests engine_opengl)
endif()
