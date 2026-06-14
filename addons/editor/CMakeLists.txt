add_library(engine_editor STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorCameraView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorCommands.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorFileNaming.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorUI.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorAssetBrowser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorScriptAssets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorMaterialLibrary.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorSerialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorPrefabWindow.cpp
)
target_include_directories(engine_editor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_compile_definitions(engine_editor
    PUBLIC
        KROM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        KROM_BUILD_DIR="${CMAKE_BINARY_DIR}"
        KROM_CMAKE_COMMAND="${CMAKE_COMMAND}"
)
target_link_libraries(engine_editor
    PUBLIC
        engine_core
        engine_asset_pipeline
        engine_gltf
        engine_lit
        engine_pbr
        engine_unlit
        engine_prefab
        engine_mesh_renderer
        engine_renderer_api
        engine_renderer_runtime_layer
)

set(KROM_IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
if(EXISTS "${KROM_IMGUI_DIR}/imgui.h")
    set(KROM_IMGUI_BACKEND_DIR "${KROM_IMGUI_DIR}/backends")

    target_sources(engine_editor PRIVATE
        ${KROM_IMGUI_DIR}/imgui.cpp
        ${KROM_IMGUI_DIR}/imgui_draw.cpp
        ${KROM_IMGUI_DIR}/imgui_tables.cpp
        ${KROM_IMGUI_DIR}/imgui_widgets.cpp
    )
    target_compile_definitions(engine_editor PUBLIC KROM_EDITOR_HAS_IMGUI=1)
    target_include_directories(engine_editor PUBLIC
        $<BUILD_INTERFACE:${KROM_IMGUI_DIR}>
        $<BUILD_INTERFACE:${KROM_IMGUI_BACKEND_DIR}>
    )

    if(WIN32 AND EXISTS "${KROM_IMGUI_BACKEND_DIR}/imgui_impl_win32.cpp")
        target_sources(engine_editor PRIVATE
            ${KROM_IMGUI_BACKEND_DIR}/imgui_impl_win32.cpp
        )
        target_compile_definitions(engine_editor PUBLIC KROM_IMGUI_HAS_WIN32=1)
        message(STATUS "  Editor: ImGui Win32-Backend eingebunden")
    endif()

    if(KROM_USE_GLFW AND EXISTS "${KROM_IMGUI_BACKEND_DIR}/imgui_impl_glfw.cpp")
        find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h)
        if(GLFW_INCLUDE_DIR)
            target_sources(engine_editor PRIVATE
                ${KROM_IMGUI_BACKEND_DIR}/imgui_impl_glfw.cpp
            )
            target_include_directories(engine_editor PRIVATE ${GLFW_INCLUDE_DIR})
            target_compile_definitions(engine_editor PUBLIC KROM_IMGUI_HAS_GLFW=1)
            message(STATUS "  Editor: ImGui GLFW-Backend eingebunden")
        endif()
    endif()

    if(KROM_BUILD_DX11 AND WIN32 AND EXISTS "${KROM_IMGUI_BACKEND_DIR}/imgui_impl_dx11.cpp")
        target_sources(engine_editor PRIVATE
            ${KROM_IMGUI_BACKEND_DIR}/imgui_impl_dx11.cpp
        )
        target_compile_definitions(engine_editor PUBLIC KROM_IMGUI_HAS_DX11=1)
        message(STATUS "  Editor: ImGui DX11-Backend eingebunden")
    endif()

    if(KROM_BUILD_OPENGL AND EXISTS "${KROM_IMGUI_BACKEND_DIR}/imgui_impl_opengl3.cpp")
        target_sources(engine_editor PRIVATE
            ${KROM_IMGUI_BACKEND_DIR}/imgui_impl_opengl3.cpp
        )
        target_compile_definitions(engine_editor PUBLIC KROM_IMGUI_HAS_OPENGL3=1)
        message(STATUS "  Editor: ImGui OpenGL3-Backend eingebunden")
    endif()

    if(KROM_BUILD_VULKAN AND EXISTS "${KROM_IMGUI_BACKEND_DIR}/imgui_impl_vulkan.cpp")
        target_sources(engine_editor PRIVATE
            ${KROM_IMGUI_BACKEND_DIR}/imgui_impl_vulkan.cpp
        )
        find_package(Vulkan QUIET)
        if(Vulkan_FOUND)
            target_include_directories(engine_editor PRIVATE ${Vulkan_INCLUDE_DIRS})
        elseif(DEFINED ENV{VULKAN_SDK})
            target_include_directories(engine_editor PRIVATE "$ENV{VULKAN_SDK}/Include")
        endif()
        target_compile_definitions(engine_editor PUBLIC KROM_IMGUI_HAS_VULKAN=1)
        message(STATUS "  Editor: ImGui Vulkan-Backend eingebunden")
    endif()

    message(STATUS "  Editor: ImGui gefunden, UI-Panels aktiviert")
else()
    message(STATUS "  Editor: ImGui nicht gefunden (third_party/imgui/imgui.h fehlt), UI-Panels deaktiviert")
endif()
