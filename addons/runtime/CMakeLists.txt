add_library(engine_addon_adapters STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/EngineAddonAdapters.cpp
)
target_include_directories(engine_addon_adapters
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_addon_adapters
    PUBLIC
        engine_core
        engine_camera
        engine_forward
        engine_lighting
        engine_mesh_renderer
        engine_particles
        engine_shadow
)