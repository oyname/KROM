set(DX11_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/DX11Device.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DX11Enumeration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DX11Converters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DX11Resources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DX11CommandList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DX11Swapchain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DX11ShaderReflector.cpp
)

if(KROM_BUILD_DX11 AND WIN32)
    add_library(engine_dx11 STATIC ${DX11_SOURCES})
    target_include_directories(engine_dx11
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    )
    target_link_libraries(engine_dx11 PUBLIC engine_core d3d11 d3dcompiler dxgi dxguid)
    target_compile_definitions(engine_dx11 PUBLIC KROM_DX11_BACKEND)
    message(STATUS "  DX11 addon: ENABLED")
else()
    add_library(engine_dx11 STATIC ${CMAKE_CURRENT_SOURCE_DIR}/DX11Stubs.cpp)
    target_include_directories(engine_dx11
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    )
    target_link_libraries(engine_dx11 PUBLIC engine_core)
    message(STATUS "  DX11 addon: stubs only")
endif()

