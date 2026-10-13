# Erwartet: third_party/cgltf/cgltf.h  (jkuhlmann/cgltf, zero dependencies)
# Download: https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/cgltf/cgltf.h")
    set(CGLTF_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/cgltf")
else()
    message(STATUS "engine_gltf: cgltf.h nicht gefunden. Addon deaktiviert.")
    message(STATUS "  -> Datei ablegen als: third_party/cgltf/cgltf.h")
    message(STATUS "  -> Download: https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h")
    return()
endif()

add_library(engine_gltf STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/GltfImporter.cpp
)

target_include_directories(engine_gltf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE
        ${CGLTF_INCLUDE_DIR}
)

target_link_libraries(engine_gltf
    PUBLIC
        engine_core
)

message(STATUS "engine_gltf: aktiviert (cgltf.h gefunden)")
