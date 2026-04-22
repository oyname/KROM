set(LIT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/LitMaterial.cpp
)

add_library(engine_lit STATIC ${LIT_SOURCES})
target_include_directories(engine_lit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_lit PUBLIC engine_core)
target_compile_definitions(engine_lit PUBLIC KROM_LIT_ADDON)
