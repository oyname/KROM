# engine_audio_data: Komponenten + Serialisierung, ohne miniaudio.
add_library(engine_audio_data STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/AudioSerialization.cpp
)
target_include_directories(engine_audio_data
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/addons/audio>
)
target_link_libraries(engine_audio_data
    PUBLIC
        engine_core
)

# engine_audio: Wiedergabe. miniaudio bleibt PRIVATE — kein oeffentlicher
# Header dieses Addons bindet es ein.
if(KROM_WITH_AUDIO AND DEFINED KROM_MINIAUDIO_DIR)
    add_library(engine_audio STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/AudioSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/MiniaudioImpl.cpp
    )
    target_include_directories(engine_audio
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/addons/audio>
        PRIVATE
            ${KROM_MINIAUDIO_DIR}
    )
    target_link_libraries(engine_audio
        PUBLIC
            engine_audio_data
            engine_core
    )
    if(MSVC)
        # Fremdcode: Warnungen der Implementierungs-TU ausblenden.
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/MiniaudioImpl.cpp
            PROPERTIES COMPILE_OPTIONS "/W0")
    else()
        set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/MiniaudioImpl.cpp
            PROPERTIES COMPILE_OPTIONS "-w")
        if(UNIX AND NOT APPLE)
            target_link_libraries(engine_audio PRIVATE dl m)
        endif()
    endif()
endif()
