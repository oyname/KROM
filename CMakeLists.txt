cmake_minimum_required(VERSION 3.22)
project(KROMEngine VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
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

option(KROM_USE_GLFW   "Build GLFW platform layer when GLFW is available" ON)

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
    # Collision
    src/collision/SceneQueries.cpp
    # Renderer
    src/renderer/MaterialSystem.cpp
    src/renderer/DeviceFactory.cpp
    src/renderer/PipelineCache.cpp
    src/renderer/RenderWorld.cpp
    src/renderer/GpuResourceRuntime.cpp
    src/renderer/ShaderCompiler.cpp
    src/renderer/ShaderRuntime.cpp
    src/renderer/ECSExtractor.cpp
    src/renderer/FeatureRegistry.cpp
    src/renderer/FrameExtractionStage.cpp
    src/renderer/FramePreparationStage.cpp
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
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)


find_package(Threads REQUIRED)
target_link_libraries(engine_core PUBLIC Threads::Threads)
target_compile_definitions(engine_core PUBLIC KROM_HAS_GLFW=0)

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
            PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                   $<INSTALL_INTERFACE:include>
            PRIVATE ${GLFW_INCLUDE_DIR}
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
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win32>)
    target_link_libraries(engine_platform_win32 PUBLIC engine_core)
    message(STATUS "  Win32 platform: ENABLED")
else()
    message(STATUS "  Win32 platform: disabled (not Windows)")
endif()

# ── AddOns ────────────────────────────────────────────────────
add_subdirectory(addons/null)
add_subdirectory(addons/dx11)
add_subdirectory(addons/opengl)
add_subdirectory(addons/forward)

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

if(TARGET engine_dx11 AND TARGET engine_platform_win32)
    add_executable(engine_dx11_render_loop examples/dx11_render_loop.cpp)
    target_link_libraries(engine_dx11_render_loop PRIVATE
        engine_core
        engine_forward
        engine_platform_win32
    )
    krom_link_self_registering_addon(engine_dx11_render_loop engine_dx11)
    target_include_directories(engine_dx11_render_loop PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/dx11
        ${CMAKE_CURRENT_SOURCE_DIR}/addons/forward
    )
    add_custom_command(TARGET engine_dx11_render_loop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets
            $<TARGET_FILE_DIR:engine_dx11_render_loop>/assets
    )
endif()

if(TARGET engine_opengl)
    if(WIN32 AND TARGET engine_platform_win32)
        add_executable(engine_opengl_render_loop examples/opengl_render_loop.cpp)
        target_link_libraries(engine_opengl_render_loop PRIVATE
            engine_core
            engine_forward
            engine_platform_win32
        )
        krom_link_self_registering_addon(engine_opengl_render_loop engine_opengl)
        target_include_directories(engine_opengl_render_loop PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/addons/opengl
            ${CMAKE_CURRENT_SOURCE_DIR}/addons/forward
        )
    elseif(TARGET engine_platform_glfw)
        add_executable(engine_opengl_render_loop examples/opengl_render_loop.cpp)
        target_link_libraries(engine_opengl_render_loop PRIVATE
            engine_core
            engine_forward
            engine_platform_glfw
        )
        krom_link_self_registering_addon(engine_opengl_render_loop engine_opengl)
        target_include_directories(engine_opengl_render_loop PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/addons/opengl
            ${CMAKE_CURRENT_SOURCE_DIR}/addons/forward
        )
    endif()

    if(TARGET engine_opengl_render_loop)
        add_custom_command(TARGET engine_opengl_render_loop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/assets
                $<TARGET_FILE_DIR:engine_opengl_render_loop>/assets
        )
    endif()
endif()

message(STATUS "===================================================")
message(STATUS "  KROM Engine v${PROJECT_VERSION}")
message(STATUS "  Build: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "===================================================")
