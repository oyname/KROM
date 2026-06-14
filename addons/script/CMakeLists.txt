add_library(engine_script STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScriptSerialization.cpp
)
target_include_directories(engine_script
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/addons/script>
)
target_link_libraries(engine_script
    PUBLIC
        engine_core
)
