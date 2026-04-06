add_library(engine_forward STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ForwardFeature.cpp
)
target_include_directories(engine_forward
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_forward PUBLIC engine_core)
