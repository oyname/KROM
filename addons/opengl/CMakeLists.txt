set(OPENGL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLCommandList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLSwapchain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLConverters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLEnumeration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GLShaderReflector.cpp
)
if(WIN32)
    list(APPEND OPENGL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLWin32Loader.cpp)
endif()

# ---------------------------------------------------------------------------
# Probe availability — never use REQUIRED so that a missing libGL/Mesa on a
# headless Linux CI machine falls through to stubs instead of aborting cmake.
# ---------------------------------------------------------------------------
set(_krom_opengl_available OFF)
if(KROM_BUILD_OPENGL)
    if(UNIX AND NOT APPLE)
        find_package(OpenGL QUIET)
        if(OpenGL_FOUND)
            set(_krom_opengl_available ON)
        else()
            message(STATUS "  OpenGL addon: disabled (OpenGL library not found; install libgl-dev / mesa-common-dev)")
        endif()
    else()
        # Windows links opengl32 directly; macOS uses framework — always available.
        set(_krom_opengl_available ON)
    endif()
endif()

if(_krom_opengl_available)
    add_library(engine_opengl STATIC ${OPENGL_SOURCES})
    target_include_directories(engine_opengl
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    )
    target_link_libraries(engine_opengl PUBLIC engine_core)
    if(UNIX AND NOT APPLE)
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
    add_library(engine_opengl STATIC ${CMAKE_CURRENT_SOURCE_DIR}/OpenGLStubs.cpp)
    target_include_directories(engine_opengl
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    )
    target_link_libraries(engine_opengl PUBLIC engine_core)
    message(STATUS "  OpenGL addon: stubs only")
endif()