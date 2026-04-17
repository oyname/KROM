add_library(engine_camera STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/CameraViewBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CameraSerialization.cpp
)
target_include_directories(engine_camera
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)