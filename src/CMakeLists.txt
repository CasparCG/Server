cmake_minimum_required(VERSION 3.16)

# Project settings
project("CasparCG Server")
option(BUILD_TESTING "Build regression tests" OFF)
include(CTest)
set(CONFIG_VERSION_MAJOR 2)
set(CONFIG_VERSION_MINOR 6)
set(CONFIG_VERSION_BUG 0)
set(CONFIG_VERSION_TAG "Dev")

# support for language servers (eg, clangd or ccls)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CASPARCG_DOWNLOAD_MIRROR https://github.com/CasparCG/dependencies/releases/download/ CACHE STRING "Source/mirror to use for external dependencies")
set(CASPARCG_DOWNLOAD_CACHE ${CMAKE_CURRENT_BINARY_DIR}/external CACHE STRING "Download cache directory for cmake ExternalProjects")

option(ENABLE_HTML "Enable HTML module, require CEF" ON)

set(DIAG_FONT_PATH "LiberationMono-Regular.ttf" CACHE STRING
    "Path to font that will be used to load diag font at runtime. By default
    this loads the font distributed with the application from the working
    directory. It can be set to an absolute path to instead load a font from
    the system."
)
add_compile_definitions("DIAG_FONT_PATH=\"${DIAG_FONT_PATH}\"")

# Add custom cmake modules path
set(CASPARCG_PATCH_DIR ${CMAKE_SOURCE_DIR}/CMakeModules/patches)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/CMakeModules)

# Determine the git hash
find_package(Git)
set(CONFIG_VERSION_GIT_HASH "N/A")
if(DEFINED ENV{GIT_HASH} AND NOT $ENV{GIT_HASH} STREQUAL "")
    set(CONFIG_VERSION_GIT_HASH "$ENV{GIT_HASH}")
elseif(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/../.git")
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --verify --short HEAD
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/../"
            OUTPUT_VARIABLE CONFIG_VERSION_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

cmake_host_system_information(RESULT CONFIG_CPU_COUNT QUERY NUMBER_OF_PHYSICAL_CORES)

include(CasparCG_Util)
configure_file("${PROJECT_SOURCE_DIR}/version.tmpl" "${CMAKE_BINARY_DIR}/generated/version.h")

if(MSVC)
    include(Bootstrap_Windows)
else ()
    include(Bootstrap_Linux)

    # Output build settings
    message(STATUS "Target architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "CPU Count: ${CONFIG_CPU_COUNT}")
    message(STATUS "Target build type: ${CMAKE_BUILD_TYPE}")
    message(STATUS "Build Version: ${CONFIG_VERSION_MAJOR}.${CONFIG_VERSION_MINOR}.${CONFIG_VERSION_BUG} ${CONFIG_VERSION_TAG} (Revision: ${CONFIG_VERSION_GIT_HASH})")
endif ()

IF(ENABLE_VULKAN)
    add_definitions(-DENABLE_VULKAN)
ENDIF()

include_directories("${CMAKE_BINARY_DIR}/generated")

add_subdirectory(tools)
add_subdirectory(accelerator)
add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(modules)
add_subdirectory(protocol)
add_subdirectory(shell)
