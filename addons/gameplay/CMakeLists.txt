# engine_gameplay: gemeinsame Spielschleife fuer Runtime (krom.h) und Play im Editor.
add_library(engine_gameplay STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/GameSimulation.cpp
)
target_include_directories(engine_gameplay
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_link_libraries(engine_gameplay
    PUBLIC
        engine_core
        engine_script
        engine_text
        engine_particles_data
        engine_physics_data
        engine_audio_data
)
if(TARGET engine_physics)
    target_link_libraries(engine_gameplay PUBLIC engine_physics)
    target_compile_definitions(engine_gameplay PRIVATE KROM_GAMEPLAY_HAS_PHYSICS=1)
endif()
if(TARGET engine_audio)
    target_link_libraries(engine_gameplay PUBLIC engine_audio)
    target_compile_definitions(engine_gameplay PRIVATE KROM_GAMEPLAY_HAS_AUDIO=1)
endif()
