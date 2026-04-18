set(UNLIT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/UnlitMaterial.cpp
)

add_library(engine_unlit STATIC ${UNLIT_SOURCES})
target_include_directories(engine_unlit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_unlit PUBLIC engine_core)
target_compile_definitions(engine_unlit PUBLIC KROM_UNLIT_ADDON)