add_library(engine_forward STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ForwardFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ForwardPlusLighting.cpp
)
target_include_directories(engine_forward
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_forward
    PUBLIC
        engine_rendergraph_layer
        engine_renderer_runtime_layer
        engine_shader_layer
        engine_material_layer
)
