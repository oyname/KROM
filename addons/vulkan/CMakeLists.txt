set(VULKAN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/VulkanDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VulkanEnumeration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VulkanConverters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VulkanCommandList.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VulkanSwapchain.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VKShaderReflector.cpp
)

# ---------------------------------------------------------------------------
# Probe availability — never use REQUIRED so that a machine without the
# Vulkan SDK falls through to stubs instead of aborting cmake.
# ---------------------------------------------------------------------------
set(_krom_vulkan_available OFF)
if(KROM_BUILD_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(_krom_vulkan_available ON)
    else()
        message(STATUS "  Vulkan addon: disabled (Vulkan SDK not found; set VULKAN_SDK or install vulkan-dev)")
    endif()
endif()

if(_krom_vulkan_available)
    add_library(engine_vulkan STATIC ${VULKAN_SOURCES})
    target_include_directories(engine_vulkan
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    )
    target_link_libraries(engine_vulkan PUBLIC engine_core Vulkan::Vulkan)
    target_compile_definitions(engine_vulkan PUBLIC KROM_VULKAN_BACKEND)
    message(STATUS "  Vulkan addon: ENABLED")
else()
    add_library(engine_vulkan STATIC ${CMAKE_CURRENT_SOURCE_DIR}/VulkanStubs.cpp)
    target_include_directories(engine_vulkan
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    )
    target_link_libraries(engine_vulkan PUBLIC engine_core)
    message(STATUS "  Vulkan addon: stubs only")
endif()