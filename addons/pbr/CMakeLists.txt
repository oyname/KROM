set(PBR_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/PbrMaterial.cpp
)

add_library(engine_pbr STATIC ${PBR_SOURCES})
target_include_directories(engine_pbr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_pbr PUBLIC engine_core)
target_compile_definitions(engine_pbr PUBLIC KROM_PBR_ADDON)