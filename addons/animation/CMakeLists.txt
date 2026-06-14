add_library(engine_animation STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/AnimationSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SkinningSystem.cpp
)
target_include_directories(engine_animation
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/addons/animation>
)
target_link_libraries(engine_animation
    PUBLIC
        engine_core
        engine_renderer_api
)
