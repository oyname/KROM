# engine_postprocess: post-process volumes (component, blending, serialization).
# No GPU code: the hosts blend the values every frame and hand them to the
# forward, GTAO, SSR and contact shadow add-ons.
add_library(engine_postprocess STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/PostProcessBlend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PostProcessSerialization.cpp
)
target_include_directories(engine_postprocess
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_postprocess
    PUBLIC
        engine_core
)
