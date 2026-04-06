add_library(engine_null STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/NullDevice.cpp
)
target_include_directories(engine_null
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_null PUBLIC engine_core)
