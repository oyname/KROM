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

add_library(engine_core STATIC
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
    # Assets
    src/assets/AssetPipeline.cpp
    src/assets/ImageDecoder.cpp
    src/assets/TextureImporter.cpp
    src/assets/MeshTangents.cpp
    # Collision
    src/collision/SceneQueries.cpp
    # Renderer
    src/renderer/ShaderParameterLayout.cpp
    src/renderer/ParameterBlob.cpp
    src/renderer/MaterialSystem.cpp
    
    src/renderer/MaterialFeatureEval.cpp
    src/renderer/MaterialCBLayout.cpp
    src/renderer/RenderPassRegistry.cpp
    src/renderer/DeviceFactory.cpp
    src/renderer/PipelineCache.cpp
    src/renderer/RenderWorld.cpp
    src/renderer/GpuResourceRuntime.cpp
    src/renderer/ShaderCompiler.cpp
    src/renderer/ShaderCompilerShared.cpp
    src/renderer/ShaderCompilerD3D.cpp
    src/renderer/ShaderCompilerDXC.cpp
    src/renderer/ShaderCompilerSPIRV.cpp
    src/renderer/ShaderCompilerGLSL.cpp
    src/renderer/ShaderRuntime.cpp
    src/renderer/ShaderRuntimeResources.cpp
    src/renderer/ShaderRuntimeAssets.cpp
    src/renderer/ShaderRuntimeMaterials.cpp
    src/renderer/IBLBaker.cpp
    src/renderer/IBLCacheSerializer.cpp
    src/renderer/IBLResourceLoader.cpp
    src/renderer/EnvironmentSystem.cpp
    src/renderer/ECSExtractor.cpp
    src/renderer/FeatureRegistry.cpp
    src/renderer/FrameExtractionStage.cpp
    src/renderer/FrameConstantStage.cpp
    src/renderer/FrameShaderStage.cpp
    src/renderer/FrameUploadStage.cpp
    src/renderer/FrameGraphStage.cpp
    src/renderer/FrameExecutionStage.cpp
    src/renderer/RenderFrameOrchestrator.cpp
    # RenderGraph
    src/rendergraph/RenderGraph.cpp
    src/rendergraph/ResourceAliaser.cpp
    # Platform
    src/platform/Window.cpp
    src/platform/StdFilesystem.cpp
    # Renderer runtime
    src/renderer/RenderSystem.cpp
    src/renderer/PlatformRenderLoop.cpp
)

target_include_directories(engine_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer  # internal headers (FloatConvert.hpp, …)
)

find_package(Threads REQUIRED)
target_link_libraries(engine_core PUBLIC Threads::Threads)
target_compile_definitions(engine_core PUBLIC KROM_HAS_GLFW=0)

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
    target_link_libraries(engine_core PRIVATE ${KROM_SHADERC_TARGET})
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

target_compile_definitions(engine_core PUBLIC KROM_HAS_SHADERC=${KROM_HAS_SHADERC})

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
        src/platform/win32/Win32Thread.cpp
    )
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
add_subdirectory(addons/pbr)
add_subdirectory(addons/unlit)
add_subdirectory(addons/lit)
add_subdirectory(addons/shadow)

target_link_libraries(engine_core
    PUBLIC
        engine_camera
        engine_lighting
        engine_mesh_renderer
        engine_particles
)

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

# ── Examples ──────────────────────────────────────────────────
function(krom_copy_assets target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:${target}>/assets
    )
endfunction()

function(krom_add_example target platform_target backend_addon backend_define)
    add_executable(${target}
        examples/pbr_shadow_main.cpp
        examples/framework/ExampleApp.cpp
        examples/framework/PbrShadowScene.cpp
    )
    target_link_libraries(${target} PRIVATE
        engine_core
        engine_forward
        engine_pbr
        engine_shadow
        ${platform_target}
    )
    krom_link_self_registering_addon(${target} ${backend_addon})
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/forward
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/pbr
    )
    target_compile_definitions(${target} PRIVATE ${backend_define}=1)

    if(${platform_target} STREQUAL "engine_platform_win32")
        target_compile_definitions(${target} PRIVATE KROM_EXAMPLE_USE_WIN32_PLATFORM=1)
    elseif(${platform_target} STREQUAL "engine_platform_glfw")
        target_compile_definitions(${target} PRIVATE KROM_EXAMPLE_USE_GLFW_PLATFORM=1)
    endif()

    krom_copy_assets(${target})
endfunction()

if(KROM_BUILD_DX11 AND TARGET engine_dx11 AND TARGET engine_platform_win32)
    krom_add_example(krom_pbr_shadow_dx11 engine_platform_win32 engine_dx11 KROM_EXAMPLE_BACKEND_DX11)
endif()

if(KROM_BUILD_VULKAN AND TARGET engine_vulkan)
    if(TARGET engine_platform_win32)
        krom_add_example(krom_pbr_shadow_vulkan engine_platform_win32 engine_vulkan KROM_EXAMPLE_BACKEND_VULKAN)
    elseif(TARGET engine_platform_glfw)
        krom_add_example(krom_pbr_shadow_vulkan engine_platform_glfw engine_vulkan KROM_EXAMPLE_BACKEND_VULKAN)
    endif()
endif()

if(KROM_BUILD_OPENGL AND TARGET engine_opengl)
    if(TARGET engine_platform_win32)
        krom_add_example(krom_pbr_shadow_opengl engine_platform_win32 engine_opengl KROM_EXAMPLE_BACKEND_OPENGL)
    elseif(TARGET engine_platform_glfw)
        krom_add_example(krom_pbr_shadow_opengl engine_platform_glfw engine_opengl KROM_EXAMPLE_BACKEND_OPENGL)
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
    tests/test_renderer.cpp
    tests/test_rendergraph.cpp
    tests/test_platform.cpp
    tests/test_serialization.cpp
    tests/test_assets_collision.cpp
    tests/test_events.cpp 
)
target_include_directories(krom_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/forward
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/null
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/pbr
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/unlit
    ${CMAKE_CURRENT_SOURCE_DIR}/addons/lit
)

target_link_libraries(krom_tests PRIVATE
    engine_core
    engine_forward
    engine_pbr
    engine_unlit
    engine_lit
    engine_shadow
)
krom_link_self_registering_addon(krom_tests engine_null)
krom_link_self_registering_addon(krom_tests engine_dx11)
krom_link_self_registering_addon(krom_tests engine_opengl)
endif()