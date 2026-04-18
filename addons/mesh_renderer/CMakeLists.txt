add_library(engine_mesh_renderer STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/MeshRendererFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MeshRendererExtraction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MeshRendererSerialization.cpp
)
target_include_directories(engine_mesh_renderer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
