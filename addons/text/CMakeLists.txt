add_library(engine_text STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/TextRenderer.cpp
)

target_include_directories(engine_text
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    PRIVATE
        # imstb_truetype.h / imstb_rectpack.h (stb_truetype 1.26, statisch eingebunden)
        ${CMAKE_SOURCE_DIR}/third_party/imgui
)

target_link_libraries(engine_text
    PUBLIC
        engine_renderer_api
        engine_shader_layer
        engine_asset_pipeline
)

target_compile_definitions(engine_text PUBLIC KROM_TEXT_ADDON)
