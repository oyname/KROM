add_library(engine_outline STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/OutlineFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OutlinePass.cpp
)
target_include_directories(engine_outline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_outline
    PUBLIC
        engine_core
        engine_rendergraph_layer
        engine_renderer_runtime_layer
        engine_shader_layer
        engine_forward
)
