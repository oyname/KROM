# engine_particles_data: Komponenten, Serialisierung und CPU-Simulation, ohne GPU.
add_library(engine_particles_data STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ParticleSerialization.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ParticleSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ParticleScriptService.cpp
)
target_include_directories(engine_particles_data
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_particles_data
    PUBLIC
        engine_core
)

# engine_particles: drawing (billboards in the particle pass); optional via
# KROM_WITH_PARTICLES. Without it particles are simulated but not drawn.
if(NOT KROM_WITH_PARTICLES)
    return()
endif()
add_library(engine_particles STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ParticleRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ParticleView.cpp
)
target_include_directories(engine_particles
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_particles
    PUBLIC
        engine_particles_data
        engine_renderer_api
        engine_shader_layer
        engine_asset_pipeline
)
