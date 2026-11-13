# engine_probes: local reflection probes (component, runtime, bake, .kprobe).
add_library(engine_probes STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeBake.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeBoxFit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeHash.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeSerialization.cpp
)
target_include_directories(engine_probes
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_probes
    PUBLIC
        engine_core
        engine_environment_layer
        engine_rendergraph_layer
        engine_renderer_runtime_layer
        engine_renderer_orchestration_layer
        engine_shader_layer
        engine_forward
        engine_lighting
        engine_mesh_renderer
)
