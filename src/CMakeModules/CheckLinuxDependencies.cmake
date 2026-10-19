# Build a small system-library compile/link probe instead of the CasparCG server.
message(STATUS "Dependency check compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Dependency check linker: ${CMAKE_LINKER}")
execute_process(COMMAND "${CMAKE_LINKER}" --version COMMAND_ERROR_IS_FATAL ANY)

add_executable(casparcg_dependency_check "${CMAKE_CURRENT_LIST_DIR}/dependency_check.cpp")
set_target_properties(casparcg_dependency_check PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(casparcg_dependency_check PRIVATE ${Boost_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(casparcg_dependency_check PRIVATE
    ${Boost_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    OpenGL::GL OpenGL::OpenGL OpenGL::EGL GLEW::glew
    TBB::tbb TBB::tbbmalloc OpenAL::OpenAL
    sfml-graphics ${X11_LIBRARIES}
    dl icui18n icuuc z pthread
)
# Retain every shared library in the probe and check its transitive symbol resolution.
target_link_options(casparcg_dependency_check PRIVATE -Wl,--no-as-needed -Wl,--no-allow-shlib-undefined)
if(ENABLE_HTML)
    if(USE_SYSTEM_CEF)
        if(TARGET CEF::Library)
            # Distro CEF wrappers can be source targets; keep the probe limited to installed libraries.
            target_link_libraries(casparcg_dependency_check PRIVATE CEF::Library)
        else()
            target_link_libraries(casparcg_dependency_check PRIVATE CEF::CEF)
        endif()
    else()
        message(STATUS "Bundled CEF linkage is checked only by a full build, not by this dependency probe")
    endif()
endif()

message(STATUS "Configured system dependency compile/link probe; CasparCG itself will not be compiled")
