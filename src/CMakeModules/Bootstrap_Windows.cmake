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

set(BOOST_USE_PRECOMPILED ON CACHE BOOL "Use precompiled boost")
set(ENABLE_VULKAN OFF CACHE BOOL "Enable Vulkan support")

set(CASPARCG_RUNTIME_DEPENDENCIES_RELEASE "" CACHE INTERNAL "")
set(CASPARCG_RUNTIME_DEPENDENCIES_DEBUG "" CACHE INTERNAL "")
set(CASPARCG_RUNTIME_DEPENDENCIES_RELEASE_DIRS "" CACHE INTERNAL "")
set(CASPARCG_RUNTIME_DEPENDENCIES_DEBUG_DIRS "" CACHE INTERNAL "")

function(casparcg_add_runtime_dependency FILE_TO_COPY)
	if ("${ARGV1}" STREQUAL "Release" OR NOT ARGV1)
		set(CASPARCG_RUNTIME_DEPENDENCIES_RELEASE "${CASPARCG_RUNTIME_DEPENDENCIES_RELEASE}" "${FILE_TO_COPY}" CACHE INTERNAL "")
	endif()
	if ("${ARGV1}" STREQUAL "Debug" OR NOT ARGV1)
		set(CASPARCG_RUNTIME_DEPENDENCIES_DEBUG "${CASPARCG_RUNTIME_DEPENDENCIES_DEBUG}" "${FILE_TO_COPY}" CACHE INTERNAL "")
	endif()
endfunction()
function(casparcg_add_runtime_dependency_dir FILE_TO_COPY)
	if ("${ARGV1}" STREQUAL "Release" OR NOT ARGV1)
		set(CASPARCG_RUNTIME_DEPENDENCIES_RELEASE_DIRS "${CASPARCG_RUNTIME_DEPENDENCIES_RELEASE_DIRS}" "${FILE_TO_COPY}" CACHE INTERNAL "")
	endif()
	if ("${ARGV1}" STREQUAL "Debug" OR NOT ARGV1)
		set(CASPARCG_RUNTIME_DEPENDENCIES_DEBUG_DIRS "${CASPARCG_RUNTIME_DEPENDENCIES_DEBUG_DIRS}" "${FILE_TO_COPY}" CACHE INTERNAL "")
	endif()
endfunction()
function(casparcg_add_runtime_dependency_from_target TARGET)
	get_target_property(_runtime_lib_name ${TARGET} IMPORTED_LOCATION_RELEASE)
	if (NOT "${_runtime_lib_name}" STREQUAL "")
		set(CASPARCG_RUNTIME_DEPENDENCIES_RELEASE "${CASPARCG_RUNTIME_DEPENDENCIES_RELEASE}" "${_runtime_lib_name}" CACHE INTERNAL "")
	endif()

	get_target_property(_runtime_lib_name ${TARGET} IMPORTED_LOCATION_DEBUG)
	if (NOT "${_runtime_lib_name}" STREQUAL "")
		set(CASPARCG_RUNTIME_DEPENDENCIES_DEBUG "${CASPARCG_RUNTIME_DEPENDENCIES_DEBUG}" "${_runtime_lib_name}" CACHE INTERNAL "")
	endif()
endfunction()

casparcg_add_runtime_dependency("${PROJECT_SOURCE_DIR}/shell/casparcg.config")
casparcg_add_runtime_dependency_dir("${PROJECT_SOURCE_DIR}/shell/template")

# BOOST
casparcg_add_external_project(boost)
if (BOOST_USE_PRECOMPILED)
	ExternalProject_Add(boost
	URL ${CASPARCG_DOWNLOAD_MIRROR}/boost/boost_1_83_0-win32-x64-debug-release.zip
	URL_HASH MD5=0b9990a24259867c8c04ae30c423f86b
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
	INSTALL_COMMAND ""
	)
	ExternalProject_Get_Property(boost SOURCE_DIR)
	set(BOOST_INCLUDE_PATH "${SOURCE_DIR}/include/boost-1_83")
	link_directories("${SOURCE_DIR}/lib")
else ()
	set(BOOST_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/boost-install)
	ExternalProject_Add(boost
	URL ${CASPARCG_DOWNLOAD_MIRROR}/boost/boost_1_83_0.zip
	URL_HASH MD5=03d5aea72401ffed848cb5daf8cd2b9b
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
	BUILD_IN_SOURCE 1
	CONFIGURE_COMMAND ./bootstrap.bat
		--with-libraries=filesystem
		--with-libraries=locale
		--with-libraries=log
		--with-libraries=log_setup
		--with-libraries=regex
		--with-libraries=system
		--with-libraries=thread
	BUILD_COMMAND ./b2 install debug release --prefix=${BOOST_INSTALL_DIR} link=static threading=multi runtime-link=shared -j ${CONFIG_CPU_COUNT}
	INSTALL_COMMAND ""
	)
	set(BOOST_INCLUDE_PATH "${BOOST_INSTALL_DIR}/include/boost-1_83")
	link_directories("${BOOST_INSTALL_DIR}/lib")
endif ()
add_definitions( -DBOOST_CONFIG_SUPPRESS_OUTDATED_MESSAGE )
add_definitions( -DBOOST_COROUTINES_NO_DEPRECATION_WARNING )
add_definitions( -DBOOST_LOCALE_HIDE_AUTO_PTR )

# FFMPEG
casparcg_add_external_project(ffmpeg-lib)
ExternalProject_Add(ffmpeg-lib
	URL ${CASPARCG_DOWNLOAD_MIRROR}/ffmpeg/ffmpeg-8.1.2-full_build-shared.7z
	URL_HASH SHA256=cba748035c21ce1431d0823c7a3a711f38616f89f87a265dceddf9b7f6749d2d
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
	INSTALL_COMMAND ""
)
ExternalProject_Get_Property(ffmpeg-lib SOURCE_DIR)
set(FFMPEG_INCLUDE_PATH "${SOURCE_DIR}/include")
set(FFMPEG_BIN_PATH "${SOURCE_DIR}/bin")
link_directories("${SOURCE_DIR}/lib")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avcodec-62.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avdevice-62.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avfilter-11.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avformat-62.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avutil-60.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/swresample-6.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/swscale-9.dll")
# for scanner:
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/ffmpeg.exe")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/ffprobe.exe")

get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

set(EXTERNAL_CMAKE_ARGS "")
if (is_multi_config)
	set(EXTERNAL_CMAKE_ARGS "-DCMAKE_BUILD_TYPE:STRING=$<CONFIG>")
else()
	set(EXTERNAL_CMAKE_ARGS "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}")
endif ()

# TBB
FetchContent_Declare(tbb
	URL ${CASPARCG_DOWNLOAD_MIRROR}/tbb/oneapi-tbb-2022.3.0-win.zip
	URL_HASH SHA256=e1b2373f25558bf47d16b4c89cf0a31e6689aaf7221400d209e8527afc7c9eee
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
)
FetchContent_MakeAvailable(tbb)

list(APPEND CMAKE_PREFIX_PATH ${tbb_SOURCE_DIR}/lib/cmake/tbb)
find_package(tbb REQUIRED)

casparcg_add_runtime_dependency_from_target(TBB::tbb)
casparcg_add_runtime_dependency_from_target(TBB::tbbmalloc)
casparcg_add_runtime_dependency_from_target(TBB::tbbmalloc_proxy)

# GLEW
FetchContent_Declare(glew
	URL ${CASPARCG_DOWNLOAD_MIRROR}/glew/glew-2.2.0-win32.zip
	URL_HASH MD5=1feddfe8696c192fa46a0df8eac7d4bf
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
)
FetchContent_MakeAvailable(glew)

add_library(GLEW::glew INTERFACE IMPORTED)
target_include_directories(GLEW::glew INTERFACE ${glew_SOURCE_DIR}/include)
target_link_directories(GLEW::glew INTERFACE ${glew_SOURCE_DIR}/lib/Release/x64)
target_link_libraries(GLEW::glew INTERFACE glew32)
casparcg_add_runtime_dependency("${glew_SOURCE_DIR}/bin/Release/x64/glew32.dll")

IF(ENABLE_VULKAN)
	find_package(Vulkan REQUIRED)

	FetchContent_Declare(vk_bootstrap
			URL ${CASPARCG_DOWNLOAD_MIRROR}/vk-bootstrap/vk-bootstrap-1.4.328.zip
			URL_HASH SHA256=10f257c30a0a49d30b28a72cf3a7942d93a61f977adaa04bee29304c6506dc12
			DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
			)
	FetchContent_MakeAvailable(vk_bootstrap)

	FetchContent_Declare(vma
			URL ${CASPARCG_DOWNLOAD_MIRROR}/VulkanMemoryAllocator/VulkanMemoryAllocator-3.3.0.zip
			URL_HASH SHA256=81755d8fcb411b97292c6682e828501315db319374c7c34ba6e1226452c6c392
			DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
	)
	FetchContent_MakeAvailable(vma)
ENDIF()

# SFML
FetchContent_Declare(sfml
	URL ${CASPARCG_DOWNLOAD_MIRROR}/sfml/SFML-2.6.2-windows-vc17-64-bit.zip
	URL_HASH MD5=dee0602d6f94d1843eef4d7568d2c23d
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
)
FetchContent_MakeAvailable(sfml)

list(APPEND CMAKE_PREFIX_PATH ${sfml_SOURCE_DIR}/lib/cmake/SFML)
# set(SFML_STATIC_LIBRARIES TRUE)
find_package(SFML 2 COMPONENTS graphics system window REQUIRED)

casparcg_add_runtime_dependency_from_target(sfml-graphics)
casparcg_add_runtime_dependency_from_target(sfml-system)
casparcg_add_runtime_dependency_from_target(sfml-window)

#ZLIB
casparcg_add_external_project(zlib)
ExternalProject_Add(zlib
	URL ${CASPARCG_DOWNLOAD_MIRROR}/zlib/zlib-1.3.tar.gz
	URL_HASH MD5=60373b133d630f74f4a1f94c1185a53f
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
	CMAKE_ARGS ${EXTERNAL_CMAKE_ARGS}
	INSTALL_COMMAND ""
)
ExternalProject_Get_Property(zlib SOURCE_DIR)
ExternalProject_Get_Property(zlib BINARY_DIR)
set(ZLIB_INCLUDE_PATH "${SOURCE_DIR};${BINARY_DIR}")
link_directories(${BINARY_DIR})

# OpenAL
FetchContent_Declare(openal
	URL ${CASPARCG_DOWNLOAD_MIRROR}/openal/openal-soft-1.19.1-bin.zip
	URL_HASH MD5=b78ef1ba26f7108e763f92df6bbc3fa5
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
)
FetchContent_MakeAvailable(openal)
file(COPY_FILE ${openal_SOURCE_DIR}/bin/Win64/soft_oal.dll ${openal_SOURCE_DIR}/bin/Win64/OpenAL32.dll)

add_library(OpenAL::OpenAL INTERFACE IMPORTED)
target_include_directories(OpenAL::OpenAL INTERFACE ${openal_SOURCE_DIR}/include)
target_link_directories(OpenAL::OpenAL INTERFACE ${openal_SOURCE_DIR}/libs/Win64)
target_link_libraries(OpenAL::OpenAL INTERFACE OpenAL32)
casparcg_add_runtime_dependency("${openal_SOURCE_DIR}/bin/Win64/OpenAL32.dll")

# flash template host
casparcg_add_external_project(flashtemplatehost)
ExternalProject_Add(flashtemplatehost
	URL ${CASPARCG_DOWNLOAD_MIRROR}/flash-template-host/flash-template-host-files.zip
	URL_HASH MD5=360184ce21e34d585d1d898fdd7a6bd8
	DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
	BUILD_IN_SOURCE 1
	CONFIGURE_COMMAND ""
	BUILD_COMMAND ""
	INSTALL_COMMAND ""
)
ExternalProject_Get_Property(flashtemplatehost SOURCE_DIR)
set(TEMPLATE_HOST_PATH "${SOURCE_DIR}")
# casparcg_add_runtime_dependency_dir("${TEMPLATE_HOST_PATH}")

# LIBERATION_FONTS
set(LIBERATION_FONTS_BIN_PATH "${PROJECT_SOURCE_DIR}/shell/liberation-fonts")
casparcg_add_runtime_dependency("${LIBERATION_FONTS_BIN_PATH}/LiberationMono-Regular.ttf")

# CEF
if (ENABLE_HTML)
	casparcg_add_external_project(cef)
	ExternalProject_Add(cef
		URL ${CASPARCG_DOWNLOAD_MIRROR}/cef/cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_windows64_minimal.tar.bz2
		URL_HASH SHA256=a6afba8ee3d1cc9ef64318a734dfeb5ceafc0384a46df2af37b857e7d76c0544
		DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
		# api_version must match CEF_API_VERSION below
		CMAKE_ARGS -DUSE_SANDBOX=Off -Dapi_version=15000 -DCEF_RUNTIME_LIBRARY_FLAG=/MD ${EXTERNAL_CMAKE_ARGS}
		INSTALL_COMMAND ""
	)
	ExternalProject_Get_Property(cef SOURCE_DIR)
	ExternalProject_Get_Property(cef BINARY_DIR)

    add_library(CEF::CEF INTERFACE IMPORTED)
	add_dependencies(CEF::CEF cef)
    target_compile_definitions(CEF::CEF INTERFACE CEF_API_VERSION=15000)
    target_include_directories(CEF::CEF INTERFACE
        "${SOURCE_DIR}"
    )

	set(CEF_RESOURCE_PATH ${SOURCE_DIR}/Resources)
	set(CEF_BIN_PATH ${SOURCE_DIR}/Release)

	if (is_multi_config)
	    target_link_libraries(CEF::CEF INTERFACE
			${SOURCE_DIR}/Release/libcef.lib
			optimized ${BINARY_DIR}/libcef_dll_wrapper/Release/libcef_dll_wrapper.lib
			debug ${BINARY_DIR}/libcef_dll_wrapper/Debug/libcef_dll_wrapper.lib)
	else()
		link_directories(${SOURCE_DIR}/Release ${BINARY_DIR}/libcef_dll_wrapper)
		target_link_libraries(CEF::CEF INTERFACE
			libcef.lib
			libcef_dll_wrapper.lib)
	endif()

	casparcg_add_runtime_dependency_dir("${CEF_RESOURCE_PATH}/locales")
	casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/chrome_100_percent.pak")
	casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/chrome_200_percent.pak")
	casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/resources.pak")
	casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/icudtl.dat")

	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/v8_context_snapshot.bin")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/libcef.dll")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/chrome_elf.dll")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/d3dcompiler_47.dll")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/dxcompiler.dll")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/dxil.dll")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/libEGL.dll")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/libGLESv2.dll")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/vk_swiftshader.dll")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/vk_swiftshader_icd.json")
	casparcg_add_runtime_dependency("${CEF_BIN_PATH}/vulkan-1.dll")
endif ()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT casparcg)

add_definitions(-DUNICODE)
add_definitions(-D_UNICODE)
add_definitions(-DCASPAR_SOURCE_PREFIX="${CMAKE_CURRENT_SOURCE_DIR}")
add_definitions(-D_WIN32_WINNT=0x0A00) # Minimum windows 10

# TODO: recompile boost to avoid this
add_compile_definitions(BOOST_USE_WINAPI_VERSION=0x0601)  # Boost ABI: must match prebuilt deps

# ignore boost deprecated headers, as these are often reported inside boost
add_definitions("-DBOOST_ALLOW_DEPRECATED_HEADERS")

# Ensure /EHsc is not defined as it clashes with EHa below
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHa /Zi /W4 /WX /MP /fp:fast /Zm192 /FIcommon/compiler/vs/disable_silly_warnings.h")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}	/D TBB_USE_ASSERT=1 /D TBB_USE_DEBUG /bigobj")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}	/Oi /arch:AVX /Ot /Gy /bigobj")
