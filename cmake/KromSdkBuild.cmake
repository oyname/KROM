# Build identity is shared by the editor, its immutable libraries and consumers.
option(KROM_BUILD_SDK "Build the distributable Windows x64 Release SDK" OFF)
if(NOT KROM_BUILD_SDK)
    return()
endif()
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "The first Krom SDK supports Windows x64 with MSVC only")
endif()
if(CMAKE_CONFIGURATION_TYPES OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "KROM_BUILD_SDK requires a single-config Release build (Ninja)")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.30 OR NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.50)
    message(FATAL_ERROR "Supported SDK compilers: MSVC 19.30 through 19.49 (v143)")
endif()
set(KROM_USE_GLFW OFF CACHE BOOL "Use GLFW" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_shaderc TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_unofficial-shaderc TRUE)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
foreach(_flags CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_RELEASE CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_RELEASE CMAKE_STATIC_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS_RELEASE)
    if("${${_flags}}" MATCHES "[-/](GL|LTCG)([: ;]|$)")
        message(FATAL_ERROR "${_flags} contains /GL or /LTCG; SDK libraries must support newer toolsets")
    endif()
endforeach()
file(GLOB_RECURSE _krom_identity_files RELATIVE "${CMAKE_SOURCE_DIR}" CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/*.hpp" "${CMAKE_SOURCE_DIR}/include/*.h"
    "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_SOURCE_DIR}/addons/*.cpp" "${CMAKE_SOURCE_DIR}/addons/*.hpp"
    "${CMAKE_SOURCE_DIR}/app/*.cpp" "${CMAKE_SOURCE_DIR}/app/*.hpp"
    "${CMAKE_SOURCE_DIR}/cmake/*.cmake" "${CMAKE_SOURCE_DIR}/cmake/*.in"
    "${CMAKE_SOURCE_DIR}/templates/project/*")
list(SORT _krom_identity_files)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_krom_identity_files})
set(_krom_identity "${PROJECT_VERSION};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_RELEASE};${KROM_WITH_PHYSICS};${KROM_WITH_AUDIO};${KROM_WITH_PARTICLES};${KROM_BUILD_DX11};${KROM_BUILD_VULKAN}")
foreach(_file IN LISTS _krom_identity_files)
    file(SHA256 "${CMAKE_SOURCE_DIR}/${_file}" _hash)
    string(APPEND _krom_identity "${_file}:${_hash};")
endforeach()
file(SHA256 "${CMAKE_SOURCE_DIR}/CMakeLists.txt" _krom_cmake_hash)
string(SHA256 KROM_SDK_BUILD_ID "${_krom_identity};${_krom_cmake_hash}")
file(WRITE "${CMAKE_BINARY_DIR}/compiler-version.cpp" "KROM_COMPILER_VERSION _MSC_FULL_VER\n")
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /nologo /EP /TP "${CMAKE_BINARY_DIR}/compiler-version.cpp"
    OUTPUT_VARIABLE _compiler_result RESULT_VARIABLE _compiler_status)
if(NOT _compiler_status EQUAL 0 OR NOT _compiler_result MATCHES "KROM_COMPILER_VERSION +([0-9]+)")
    message(FATAL_ERROR "Cannot read _MSC_FULL_VER for the SDK ABI contract")
endif()
set(KROM_SDK_COMPILER_VERSION "${CMAKE_MATCH_1}")
string(SHA256 KROM_SDK_BUILD_ID "${KROM_SDK_BUILD_ID};${KROM_SDK_COMPILER_VERSION}")
# The supported range above is the v143 family. Compiler paths can contain
# 8.3 names; folder parsing is not a reliable toolset identity.
set(KROM_SDK_TOOLSET "v143")
message(STATUS "Krom SDK build ID: ${KROM_SDK_BUILD_ID}, MSVC ${KROM_SDK_COMPILER_VERSION}")
