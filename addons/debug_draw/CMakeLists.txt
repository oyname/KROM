add_library(engine_debug_draw STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/DebugDraw.cpp
)

target_include_directories(engine_debug_draw
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)


target_link_libraries(engine_debug_draw
    PUBLIC
        engine_renderer_api
        engine_shader_layer
        engine_asset_pipeline
)

target_compile_definitions(engine_debug_draw PUBLIC KROM_DEBUG_DRAW_ADDON)
