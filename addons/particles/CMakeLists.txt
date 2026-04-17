add_library(engine_particles INTERFACE)
target_include_directories(engine_particles
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)