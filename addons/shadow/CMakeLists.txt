add_library(engine_shadow STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ShadowFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ShadowExtraction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ShadowViewBuilder.cpp
)
target_include_directories(engine_shadow
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_shadow
    PUBLIC
        engine_core
        engine_lighting
)
