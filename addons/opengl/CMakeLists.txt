set(OPENGL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLCommandList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLSwapchain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLConverters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLEnumeration.cpp
)
if(WIN32)
    list(APPEND OPENGL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLWin32Loader.cpp)
endif()

if(KROM_BUILD_OPENGL)
    add_library(engine_opengl STATIC ${OPENGL_SOURCES})
    target_include_directories(engine_opengl
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    )
    target_link_libraries(engine_opengl PUBLIC engine_core)
    if(UNIX AND NOT APPLE)
        find_package(OpenGL REQUIRED)
        target_compile_definitions(engine_opengl PRIVATE GL_GLEXT_PROTOTYPES)
        target_link_libraries(engine_opengl PRIVATE OpenGL::GL)
    elseif(APPLE)
        target_compile_definitions(engine_opengl PRIVATE GL_SILENCE_DEPRECATION)
        target_link_libraries(engine_opengl PRIVATE "-framework OpenGL")
    elseif(WIN32)
        target_link_libraries(engine_opengl PRIVATE opengl32 dxgi)
    endif()
    target_compile_definitions(engine_opengl PUBLIC KROM_OPENGL_BACKEND)
    if(MSVC)
        target_compile_options(engine_opengl PRIVATE /wd4273)
    endif()
    message(STATUS "  OpenGL addon: ENABLED")
else()
    add_library(engine_opengl STATIC ${OPENGL_SOURCES})
    target_include_directories(engine_opengl
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    )
    target_link_libraries(engine_opengl PUBLIC engine_core)
    message(STATUS "  OpenGL addon: stubs only")
endif()
