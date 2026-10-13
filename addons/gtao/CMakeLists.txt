add_library(engine_gtao STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/GtaoFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GtaoPass.cpp
)
target_include_directories(engine_gtao
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_gtao
    PUBLIC
        engine_core
        engine_rendergraph_layer
        engine_renderer_runtime_layer
        engine_shader_layer
        engine_forward
)
