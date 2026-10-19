# Simulate a distro CEF package at TEST_CEF_VERSION, or its absence when unset.
if(NOT TEST_CEF_VERSION)
    set(CEF_FOUND FALSE)
    return()
endif()

set(CEF_VERSION "${TEST_CEF_VERSION}")
add_library(CEF::Library INTERFACE IMPORTED)
add_library(libcef_dll_wrapper INTERFACE)
add_library(CEF::Wrapper ALIAS libcef_dll_wrapper)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CEF REQUIRED_VARS CEF_VERSION VERSION_VAR CEF_VERSION)
