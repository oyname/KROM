add_library(engine_lighting STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/LightingFrameData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LightingExtraction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LightingSerialization.cpp
)
target_include_directories(engine_lighting
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
