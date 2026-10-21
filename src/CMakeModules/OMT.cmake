# Open Media Transport (OMT) runtime: libvmx, libomtnet and libomt.
#
# libomt is a .NET Native AOT library built on libomtnet, and it loads libvmx (a native video
# codec) at runtime. All three are cloned from GitHub at pinned revisions and compiled during
# the CasparCG build, the same way on Windows and Linux.
#
# The omt module never links against them. It loads libomt with LoadLibrary/dlopen on first use
# (see modules/omt/util/omt_util.cpp), so CasparCG still builds and starts with
# OMT_BUILD_FROM_SOURCE=OFF. OMT is then only usable once libomt has been installed some other way.
#
# Build requirements on every platform: the .NET SDK (8.0 or newer) and clang.
# Linux also needs the zlib development package, which Native AOT links against.
# The first build needs network access: git clones the sources from GitHub and dotnet restores
# its NuGet packages from nuget.org, among them the Native AOT compiler and the reference assemblies.

option(OMT_BUILD_FROM_SOURCE "Build the OMT runtime (libvmx, libomtnet, libomt) from source and bundle it" ON)

if (NOT OMT_BUILD_FROM_SOURCE)
    message(STATUS "OMT runtime will not be built, libomt must be installed separately to use OMT")
    return()
endif ()

# Change these to move to a newer upstream release. A branch name also works, for trying out
# unreleased changes, but then the build is no longer reproducible.
set(OMT_LIBVMX_REVISION    a1828cb438823ebb777dcad20f9f56b633ae8db4 CACHE STRING "libvmx git revision")
set(OMT_LIBOMTNET_REVISION 029ef4e925c68973f3289eb0c7ecfd40a161166f CACHE STRING "libomtnet git revision")
set(OMT_LIBOMT_REVISION    bb4b67be8e5964f1017592b9fc5406495e704e5d CACHE STRING "libomt git revision")

# --- Build tools ---------------------------------------------------------------------------

find_program(OMT_DOTNET_EXECUTABLE dotnet)
if (NOT OMT_DOTNET_EXECUTABLE)
    message(FATAL_ERROR "Building the OMT runtime requires the .NET SDK, but 'dotnet' was not found in PATH.\n"
                        "Install it from https://dotnet.microsoft.com/download, or configure with "
                        "-DOMT_BUILD_FROM_SOURCE=OFF to build without OMT.")
endif ()

execute_process(
    COMMAND "${OMT_DOTNET_EXECUTABLE}" --version
    OUTPUT_VARIABLE OMT_DOTNET_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if (OMT_DOTNET_VERSION VERSION_LESS 8.0)
    message(FATAL_ERROR "Building the OMT runtime requires .NET SDK 8.0 or newer, found '${OMT_DOTNET_VERSION}'.\n"
                        "Configure with -DOMT_BUILD_FROM_SOURCE=OFF to build without OMT.")
endif ()

find_program(OMT_CLANGXX_EXECUTABLE clang++)
if (NOT OMT_CLANGXX_EXECUTABLE)
    message(FATAL_ERROR "Building the OMT runtime requires clang, but 'clang++' was not found in PATH.\n"
                        "Install it (on Windows: LLVM, or the 'C++ Clang tools for Windows' Visual Studio component), "
                        "or configure with -DOMT_BUILD_FROM_SOURCE=OFF to build without OMT.")
endif ()

# The Visual Studio developer environment sets Platform=x64. dotnet reads that as a build
# property and then writes libomtnet.dll to bin/x64/Release instead of the bin/Release that
# libomt.csproj looks in, so the variable is removed for every dotnet command.
set(OMT_DOTNET_COMMAND "${CMAKE_COMMAND}" -E env --unset=Platform "${OMT_DOTNET_EXECUTABLE}")

# --- Platform ------------------------------------------------------------------------------

# The runtime identifier tells 'dotnet publish' which platform to compile libomt for.
if (MSVC)
    set(OMT_DOTNET_RUNTIME_ID win-x64)
    set(OMT_LIBRARY_SUFFIX ".dll")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(OMT_DOTNET_RUNTIME_ID linux-x64)
    set(OMT_LIBRARY_SUFFIX ".so")
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(OMT_DOTNET_RUNTIME_ID linux-arm64)
    set(OMT_LIBRARY_SUFFIX ".so")
else ()
    message(FATAL_ERROR "The OMT runtime cannot be built for ${CMAKE_SYSTEM_PROCESSOR}, "
                        "configure with -DOMT_BUILD_FROM_SOURCE=OFF to build without OMT.")
endif ()

# --- Layout --------------------------------------------------------------------------------

# libomt.csproj finds libomtnet through the relative path ../libomtnet, so those two checkouts
# have to be siblings.
set(OMT_SOURCE_ROOT "${CMAKE_BINARY_DIR}/omt-src")
set(OMT_LIBVMX_DIR "${OMT_SOURCE_ROOT}/libvmx")
set(OMT_LIBOMTNET_DIR "${OMT_SOURCE_ROOT}/libomtnet")
set(OMT_LIBOMT_DIR "${OMT_SOURCE_ROOT}/libomt")
set(OMT_LIBOMT_PUBLISH_DIR "${OMT_SOURCE_ROOT}/libomt-publish")

set(OMT_LIBVMX_LIBRARY "${OMT_LIBVMX_DIR}/build/libvmx${OMT_LIBRARY_SUFFIX}")
set(OMT_LIBOMT_LIBRARY "${OMT_LIBOMT_PUBLISH_DIR}/libomt${OMT_LIBRARY_SUFFIX}")
set(OMT_LIBOMTNET_ASSEMBLY "${OMT_LIBOMTNET_DIR}/bin/Release/netstandard2.0/libomtnet.dll")

# --- libvmx --------------------------------------------------------------------------------

# These mirror the build scripts in libvmx/build. The x86 build includes the AVX2 code, which
# the codec only calls when the CPU supports it. The ARM build uses NEON instead.
set(OMT_LIBVMX_FLAGS -O3 -std=c++17 -fdeclspec -shared)
if (MSVC)
    list(APPEND OMT_LIBVMX_FLAGS -mlzcnt -mavx2 -mbmi "-Wl,/DEF:${OMT_LIBVMX_DIR}/exports.def")
    set(OMT_LIBVMX_SOURCES vmxcodec_x86.cpp vmxcodec_avx2.cpp vmxcodec.cpp)
elseif (OMT_DOTNET_RUNTIME_ID STREQUAL "linux-x64")
    list(APPEND OMT_LIBVMX_FLAGS -fPIC -mlzcnt -mavx2 -mbmi "-Wl,-rpath,$ORIGIN")
    set(OMT_LIBVMX_SOURCES vmxcodec_x86.cpp vmxcodec_avx2.cpp vmxcodec.cpp)
else ()
    list(APPEND OMT_LIBVMX_FLAGS -fPIC "-Wl,-rpath,$ORIGIN")
    set(OMT_LIBVMX_SOURCES vmxcodec_arm.cpp vmxcodec.cpp)
endif ()
list(TRANSFORM OMT_LIBVMX_SOURCES PREPEND "${OMT_LIBVMX_DIR}/src/")

casparcg_add_external_project(libvmx)
ExternalProject_Add(libvmx
    GIT_REPOSITORY https://github.com/openmediatransport/libvmx.git
    GIT_TAG ${OMT_LIBVMX_REVISION}
    SOURCE_DIR "${OMT_LIBVMX_DIR}"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND "${OMT_CLANGXX_EXECUTABLE}" ${OMT_LIBVMX_FLAGS} ${OMT_LIBVMX_SOURCES} -o "${OMT_LIBVMX_LIBRARY}"
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${OMT_LIBVMX_LIBRARY}"
)

# --- libomtnet and libomt ------------------------------------------------------------------

# Only netstandard2.0 is needed by libomt. The solution also targets net40, which would need
# the .NET Framework reference assemblies.
casparcg_add_external_project(libomtnet)
ExternalProject_Add(libomtnet
    GIT_REPOSITORY https://github.com/openmediatransport/libomtnet.git
    GIT_TAG ${OMT_LIBOMTNET_REVISION}
    SOURCE_DIR "${OMT_LIBOMTNET_DIR}"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND ${OMT_DOTNET_COMMAND} build libomtnet.csproj -c Release -f netstandard2.0
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${OMT_LIBOMTNET_ASSEMBLY}"
)

# Publishing runs the Native AOT compiler, which turns libomt and libomtnet into one native
# shared library with C exports.
casparcg_add_external_project(libomt)
ExternalProject_Add(libomt
    GIT_REPOSITORY https://github.com/openmediatransport/libomt.git
    GIT_TAG ${OMT_LIBOMT_REVISION}
    SOURCE_DIR "${OMT_LIBOMT_DIR}"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND ${OMT_DOTNET_COMMAND} publish libomt.csproj -c Release -r ${OMT_DOTNET_RUNTIME_ID}
                  -o "${OMT_LIBOMT_PUBLISH_DIR}"
    INSTALL_COMMAND ""
    DEPENDS libomtnet
    BUILD_BYPRODUCTS "${OMT_LIBOMT_LIBRARY}"
)

# --- Bundling ------------------------------------------------------------------------------

# libomt looks for libvmx in its own directory, so both go to the same place.
# The MIT license of the three projects asks for its notice to come with the libraries. On Windows
# tools/windows/package.bat copies the texts. Linux packages for a distribution ship them their own
# way, so only the portable folder gets them here.
if (MSVC)
    casparcg_add_runtime_dependency("${OMT_LIBOMT_LIBRARY}")
    casparcg_add_runtime_dependency("${OMT_LIBVMX_LIBRARY}")
else ()
    install(FILES "${OMT_LIBOMT_LIBRARY}" "${OMT_LIBVMX_LIBRARY}" TYPE LIB)

    if ("${CASPARCG_BINARY_NAME}" STREQUAL "casparcg")
        install(FILES "${OMT_LIBVMX_DIR}/LICENSE.txt" DESTINATION licenses RENAME libvmx.txt)
        install(FILES "${OMT_LIBOMTNET_DIR}/LICENSE.txt" DESTINATION licenses RENAME libomtnet.txt)
        install(FILES "${OMT_LIBOMT_DIR}/LICENSE.txt" DESTINATION licenses RENAME libomt.txt)
    endif ()
endif ()
