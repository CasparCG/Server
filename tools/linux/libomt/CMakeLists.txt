# Builds the OMT runtime (libomt and libvmx) on its own, for the libomt packages.
#
# It uses the same CMake module as the server, so the upstream revisions are pinned in one place,
# src/CMakeModules/OMT.cmake. The result is installed into the library directory of the system.
# The build needs the same things as the server build with OMT_BUILD_FROM_SOURCE=ON: the .NET SDK,
# clang, the zlib, OpenSSL and Brotli development packages, and network access for git and NuGet.
cmake_minimum_required(VERSION 3.16)
project(libomt LANGUAGES NONE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../src/CMakeModules")

include(GNUInstallDirs)
include(ExternalProject)
include(CasparCG_Util)

set(CASPARCG_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")

# OMT.cmake installs the license texts only for the portable build of the server, the packages
# install them where the distribution wants them.
set(CASPARCG_BINARY_NAME libomt)

include(OMT)
