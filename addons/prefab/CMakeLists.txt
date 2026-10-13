add_library(engine_prefab STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Prefab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PrefabSerialization.cpp
)

target_include_directories(engine_prefab
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/addons/prefab>
)

target_link_libraries(engine_prefab
    PUBLIC
        engine_core
        engine_asset_pipeline
        engine_mesh_renderer
        engine_animation
        engine_script
)
