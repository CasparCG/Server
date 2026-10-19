# Discover Linux libraries and prepare pinned downloads for optional dependencies.
cmake_minimum_required (VERSION 3.28)

include(ExternalProject)
include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
# Prefer the new boost helper
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(ENABLE_HTML ON CACHE BOOL "Enable CEF and HTML producer")
set(USE_STATIC_BOOST OFF CACHE BOOL "Use shared library version of Boost")
set(CASPARCG_BINARY_NAME "casparcg" CACHE STRING "Custom name of the binary to build (this disables some install files)")
set(ENABLE_AVX2 OFF CACHE BOOL "Enable the AVX2 instruction set (requires a CPU that supports it)")
set(ENABLE_VULKAN OFF CACHE BOOL "Enable Vulkan support")

# Determine build (target) platform
SET (PLATFORM_FOLDER_NAME "linux")

IF (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	MESSAGE (STATUS "Setting build type to 'Release' as none was specified.")
	SET (CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
	SET_PROPERTY (CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
ENDIF ()
MARK_AS_ADVANCED (CMAKE_INSTALL_PREFIX)

if (USE_STATIC_BOOST)
	SET (Boost_USE_STATIC_LIBS ON)
endif()
find_package(Boost 1.83.0 COMPONENTS thread filesystem log_setup log locale regex date_time coroutine REQUIRED)
find_package(FFmpeg REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS OpenGL GLX EGL)
find_package(GLEW REQUIRED)
find_package(TBB REQUIRED)
find_package(OpenAL REQUIRED)
find_package(SFML 3 COMPONENTS Graphics System Window QUIET)
if(NOT SFML_FOUND)
    find_package(SFML 2 COMPONENTS graphics system window REQUIRED)
endif()

IF (ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)

    FetchContent_Declare(vk_bootstrap
            URL ${CASPARCG_DOWNLOAD_MIRROR}/vk-bootstrap/vk-bootstrap-1.4.328.tar.gz
            URL_HASH SHA256=3be0220de218dc3e692aeac552b2953860a0e0a48257f4a61c3f1c1472674744
            DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
            )
    FetchContent_MakeAvailable(vk_bootstrap)

    FetchContent_Declare(vma
            URL ${CASPARCG_DOWNLOAD_MIRROR}/VulkanMemoryAllocator/VulkanMemoryAllocator-3.3.0.tar.gz
            URL_HASH SHA256=c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e
            DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
    )
    FetchContent_MakeAvailable(vma)
endif()


find_package(X11 REQUIRED)

if (ENABLE_HTML)
    include(SelectLinuxCEF)
    if (USE_SYSTEM_CEF)
        # Preserve non-standard system CEF library paths after installation.
        set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    else()
        if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            message(FATAL_ERROR "Downloaded CEF is available only for x86-64. Install a compatible system CEF package for ${CMAKE_SYSTEM_PROCESSOR}, or explicitly disable HTML with -DENABLE_HTML=OFF.")
        endif()
        # Download prebuilt CEF; compile only its API 15000 wrapper.
        casparcg_add_external_project(cef)
        ExternalProject_Add(cef
            URL ${CASPARCG_DOWNLOAD_MIRROR}/cef/cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_linux64_minimal.tar.bz2
            URL_HASH SHA256=dd1b5cc3ac6efe659ec7e8e9b0677ba4e43216a2fefcafd5abc9340837e32d96
            DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
            CMAKE_ARGS -DUSE_SANDBOX=Off -Dapi_version=15000
                -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target libcef_dll_wrapper --config Release
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS
                "<SOURCE_DIR>/Release/libcef.so"
                "<BINARY_DIR>/libcef_dll_wrapper/libcef_dll_wrapper.a"
        )
        # CEF needs the resources next to libcef.so
        ExternalProject_Add_Step(cef copy_resources
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                "<SOURCE_DIR>/Resources"
                "<SOURCE_DIR>/Release"
                DEPENDEES build
                DEPENDERS install
        )
        ExternalProject_Get_Property(cef SOURCE_DIR)
        ExternalProject_Get_Property(cef BINARY_DIR)

        add_library(CEF::CEF INTERFACE IMPORTED)
        target_compile_definitions(CEF::CEF INTERFACE CEF_API_VERSION=15000)
        target_include_directories(CEF::CEF INTERFACE
            "${SOURCE_DIR}"
        )
        target_link_libraries(CEF::CEF INTERFACE
            # Ninja requires the library byproducts declared above.
            "${SOURCE_DIR}/Release/libcef.so"
            "${BINARY_DIR}/libcef_dll_wrapper/libcef_dll_wrapper.a"
        )

        install(DIRECTORY ${SOURCE_DIR}/Resources/locales TYPE LIB)
        install(FILES ${SOURCE_DIR}/Resources/chrome_100_percent.pak TYPE LIB)
        install(FILES ${SOURCE_DIR}/Resources/chrome_200_percent.pak TYPE LIB)
        install(FILES ${SOURCE_DIR}/Resources/icudtl.dat TYPE LIB)
        install(FILES ${SOURCE_DIR}/Resources/resources.pak TYPE LIB)

        install(FILES ${SOURCE_DIR}/Release/chrome-sandbox TYPE LIB)
        install(FILES ${SOURCE_DIR}/Release/libcef.so TYPE LIB)
        install(FILES ${SOURCE_DIR}/Release/libEGL.so TYPE LIB)
        install(FILES ${SOURCE_DIR}/Release/libGLESv2.so TYPE LIB)
        install(FILES ${SOURCE_DIR}/Release/libvk_swiftshader.so TYPE LIB)
        install(FILES ${SOURCE_DIR}/Release/libvulkan.so.1 TYPE LIB)
        install(FILES ${SOURCE_DIR}/Release/v8_context_snapshot.bin TYPE LIB)
        install(FILES ${SOURCE_DIR}/Release/vk_swiftshader_icd.json TYPE LIB)
    endif()
endif ()

SET (BOOST_INCLUDE_PATH "${Boost_INCLUDE_DIRS}")
SET (FFMPEG_INCLUDE_PATH "${FFMPEG_INCLUDE_DIRS}")

LINK_DIRECTORIES("${FFMPEG_LIBRARY_DIRS}")

SET_PROPERTY (GLOBAL PROPERTY USE_FOLDERS ON)

ADD_DEFINITIONS (-DSFML_STATIC)
ADD_DEFINITIONS (-DUNICODE)
ADD_DEFINITIONS (-D_UNICODE)
ADD_DEFINITIONS (-DGLEW_NO_GLU)
ADD_DEFINITIONS (-DGLEW_EGL)
ADD_DEFINITIONS (-D__NO_INLINE__) # Needed for precompiled headers to work
ADD_DEFINITIONS (-DBOOST_NO_SWPRINTF) # swprintf on Linux seems to always use , as decimal point regardless of C-locale or C++-locale
ADD_DEFINITIONS (-DTBB_USE_CAPTURED_EXCEPTION=1)
ADD_DEFINITIONS (-DNDEBUG) # Needed for precompiled headers to work
ADD_DEFINITIONS (-DBOOST_LOCALE_HIDE_AUTO_PTR) # Needed for C++17 in boost 1.67+


if (NOT USE_STATIC_BOOST)
	ADD_DEFINITIONS (-DBOOST_ALL_DYN_LINK)
endif()

IF (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
	ADD_COMPILE_OPTIONS (-O3) # Needed for precompiled headers to work
endif()
IF (CMAKE_SYSTEM_PROCESSOR MATCHES "(i[3-6]86|x64|x86_64|amd64|e2k)")
    ADD_COMPILE_OPTIONS (-msse3)
    ADD_COMPILE_OPTIONS (-mssse3)
    ADD_COMPILE_OPTIONS (-msse4.1)
    IF (ENABLE_AVX2)
        ADD_COMPILE_OPTIONS (-mfma)
        ADD_COMPILE_OPTIONS (-mavx)
        ADD_COMPILE_OPTIONS (-mavx2)
    ENDIF ()
ELSE ()
    ADD_COMPILE_DEFINITIONS (USE_SIMDE) # Enable OpenMP support in simde
    ADD_COMPILE_DEFINITIONS (SIMDE_ENABLE_OPENMP) # Enable OpenMP support in simde
    ADD_COMPILE_OPTIONS (-fopenmp-simd) # Enable OpenMP SIMD support
ENDIF ()

ADD_COMPILE_OPTIONS (-fnon-call-exceptions) # Allow signal handler to throw exception

ADD_COMPILE_OPTIONS (-Wno-deprecated-declarations -Wno-write-strings -Wno-multichar -Wno-cpp -Werror)

IF (ENABLE_VULKAN)
    ADD_COMPILE_OPTIONS (-Wno-nonnull -Wno-nullability-completeness)
ENDIF()

IF (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    ADD_COMPILE_OPTIONS (-Wno-terminate)
ELSEIF (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Help TBB figure out what compiler support for c++11 features
    # https://github.com/01org/tbb/issues/22
    string(REPLACE "." "0" TBB_USE_GLIBCXX_VERSION ${CMAKE_CXX_COMPILER_VERSION})
    message(STATUS "ADDING: -DTBB_USE_GLIBCXX_VERSION=${TBB_USE_GLIBCXX_VERSION}")
    add_definitions(-DTBB_USE_GLIBCXX_VERSION=${TBB_USE_GLIBCXX_VERSION})
ENDIF ()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")
