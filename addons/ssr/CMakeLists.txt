add_library(engine_ssr STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/SsrFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SsrPass.cpp
)
target_include_directories(engine_ssr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_ssr
    PUBLIC
        engine_core
        engine_rendergraph_layer
        engine_renderer_runtime_layer
        engine_shader_layer
        engine_forward
)
