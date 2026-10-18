cmake_minimum_required (VERSION 3.28)
project (shell)

join_list("${CASPARCG_MODULE_INCLUDE_STATEMENTS}" "\n" CASPARCG_MODULE_INCLUDE_STATEMENTS)
join_list("${CASPARCG_MODULE_INIT_STATEMENTS}" "\n" CASPARCG_MODULE_INIT_STATEMENTS)
join_list("${CASPARCG_MODULE_UNINIT_STATEMENTS}" "\n" CASPARCG_MODULE_UNINIT_STATEMENTS)
join_list("${CASPARCG_MODULE_COMMAND_LINE_ARG_INTERCEPTORS_STATEMENTS}" "\n" CASPARCG_MODULE_COMMAND_LINE_ARG_INTERCEPTORS_STATEMENTS)
CONFIGURE_FILE ("${PROJECT_SOURCE_DIR}/included_modules.tmpl" "${CMAKE_BINARY_DIR}/generated/included_modules.h")

set(SOURCES
		casparcg.config
		main.cpp
		server.cpp
)
set(HEADERS
		platform_specific.h
		server.h
)

if (MSVC)
	list(APPEND SOURCES
			resource.h
			shell.rc
			windows_specific.cpp
	)
else ()
	list(APPEND SOURCES
			linux_specific.cpp
	)
endif ()


add_executable(casparcg ${SOURCES} ${HEADERS})
set_target_properties(casparcg PROPERTIES
        OUTPUT_NAME "${CASPARCG_BINARY_NAME}"
    )
set_target_properties(casparcg PROPERTIES
		CXX_STANDARD 20
		CXX_STANDARD_REQUIRED ON
		CXX_EXTENSIONS OFF)
target_include_directories(casparcg PRIVATE
    ..
    ${BOOST_INCLUDE_PATH}
    )
casparcg_add_build_dependencies(casparcg)

source_group(sources ./*)

target_link_libraries(casparcg
		accelerator
		common
		core
		protocol

		"${CASPARCG_MODULE_TARGETS}"

		TBB::tbb
		TBB::tbbmalloc
		GLEW::glew
)

if (MSVC)
	target_link_libraries(casparcg
		Winmm.lib
		Ws2_32.lib
		OpenGL32.lib
		debug zlibstaticd.lib
		optimized zlibstatic.lib

		OpenAL::OpenAL
		optimized TBB::tbbmalloc_proxy

		avformat.lib
		avcodec.lib
		avutil.lib
		avfilter.lib
		avdevice.lib
		swscale.lib
		swresample.lib

		d3d9.lib
		d3d11.lib
		dxgi.lib
	)
else ()
	target_link_libraries(casparcg
		${Boost_LIBRARIES}
		OpenGL::GL
		OpenGL::OpenGL
		OpenGL::EGL
		${X11_LIBRARIES}
		${FFMPEG_LIBRARIES}
		dl
		icui18n
		icuuc
		z
		pthread
	)
endif ()

add_custom_target(casparcg_copy_dependencies ALL)
casparcg_add_build_dependencies(casparcg_copy_dependencies)

set(OUTPUT_FOLDER "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}")
add_custom_command(TARGET casparcg_copy_dependencies POST_BUILD COMMAND ${CMAKE_COMMAND} -E make_directory \"${OUTPUT_FOLDER}\")

if (MSVC)
    if (is_multi_config)
	    foreach(FILE_TO_COPY ${CASPARCG_RUNTIME_DEPENDENCIES_DEBUG})
		    get_filename_component(FOLDER_NAME "${FILE_TO_COPY}" NAME)
		    add_custom_command(
			    TARGET casparcg_copy_dependencies
			    POST_BUILD
			    COMMAND if $(ConfigurationName) == Debug (
				    echo \"Copy Debug ${FILE_TO_COPY}\" &&
				    ${CMAKE_COMMAND} -E copy \"${FILE_TO_COPY}\" \"${OUTPUT_FOLDER}/\" &&
				    ${CMAKE_COMMAND} -E copy \"${FILE_TO_COPY}\" \"${CMAKE_CURRENT_BINARY_DIR}/\"
			    )
		    )
	    endforeach(FILE_TO_COPY)
	    foreach(FILE_TO_COPY ${CASPARCG_RUNTIME_DEPENDENCIES_RELEASE})
		    get_filename_component(FOLDER_NAME "${FILE_TO_COPY}" NAME)
		    add_custom_command(
			    TARGET casparcg_copy_dependencies
			    POST_BUILD
			    COMMAND if $(ConfigurationName) NEQ Debug (
				    echo \"Copy Release ${FILE_TO_COPY}\" &&
				    ${CMAKE_COMMAND} -E copy \"${FILE_TO_COPY}\" \"${OUTPUT_FOLDER}/\" &&
				    ${CMAKE_COMMAND} -E copy \"${FILE_TO_COPY}\" \"${CMAKE_CURRENT_BINARY_DIR}/\"
			    )
		    )
	    endforeach(FILE_TO_COPY)

	    foreach(FILE_TO_COPY ${CASPARCG_RUNTIME_DEPENDENCIES_DEBUG_DIRS})
		    get_filename_component(FOLDER_NAME "${FILE_TO_COPY}" NAME)
		    add_custom_command(
			    TARGET casparcg_copy_dependencies
			    POST_BUILD
			    COMMAND if $(ConfigurationName) == Debug (
				    echo \"Copy Debug ${FILE_TO_COPY}\" &&
				    ${CMAKE_COMMAND} -E copy_directory \"${FILE_TO_COPY}\" \"${OUTPUT_FOLDER}/${FOLDER_NAME}/\" &&
				    ${CMAKE_COMMAND} -E copy_directory \"${FILE_TO_COPY}\" \"${CMAKE_CURRENT_BINARY_DIR}/${FOLDER_NAME}/\"
			    )
		    )
	    endforeach(FILE_TO_COPY)
	    foreach(FILE_TO_COPY ${CASPARCG_RUNTIME_DEPENDENCIES_RELEASE_DIRS})
		    get_filename_component(FOLDER_NAME "${FILE_TO_COPY}" NAME)
		    add_custom_command(
			    TARGET casparcg_copy_dependencies
			    POST_BUILD
			    COMMAND if $(ConfigurationName) NEQ Debug (
				    echo \"Copy Release ${FILE_TO_COPY}\" &&
				    ${CMAKE_COMMAND} -E copy_directory \"${FILE_TO_COPY}\" \"${OUTPUT_FOLDER}/${FOLDER_NAME}/\" &&
				    ${CMAKE_COMMAND} -E copy_directory \"${FILE_TO_COPY}\" \"${CMAKE_CURRENT_BINARY_DIR}/${FOLDER_NAME}/\"
			    )
		    )
	    endforeach(FILE_TO_COPY)
    elseif(${CMAKE_BUILD_TYPE} MATCHES "Debug")
	    foreach(FILE_TO_COPY ${CASPARCG_RUNTIME_DEPENDENCIES_DEBUG})
		    get_filename_component(FOLDER_NAME "${FILE_TO_COPY}" NAME)
		    add_custom_command(
			    TARGET casparcg_copy_dependencies
			    POST_BUILD
			    COMMAND echo \"Copy Debug ${FILE_TO_COPY}\" &&
				    ${CMAKE_COMMAND} -E copy \"${FILE_TO_COPY}\" \"${OUTPUT_FOLDER}/\" &&
				    ${CMAKE_COMMAND} -E copy \"${FILE_TO_COPY}\" \"${CMAKE_CURRENT_BINARY_DIR}/\"
		    )
	    endforeach(FILE_TO_COPY)

	    foreach(FILE_TO_COPY ${CASPARCG_RUNTIME_DEPENDENCIES_DEBUG_DIRS})
		    get_filename_component(FOLDER_NAME "${FILE_TO_COPY}" NAME)
		    add_custom_command(
			    TARGET casparcg_copy_dependencies
			    POST_BUILD
			    COMMAND echo \"Copy Debug ${FILE_TO_COPY}\" &&
				    ${CMAKE_COMMAND} -E copy_directory \"${FILE_TO_COPY}\" \"${OUTPUT_FOLDER}/${FOLDER_NAME}/\" &&
				    ${CMAKE_COMMAND} -E copy_directory \"${FILE_TO_COPY}\" \"${CMAKE_CURRENT_BINARY_DIR}/${FOLDER_NAME}/\"
		    )
	    endforeach(FILE_TO_COPY)
    else ()
	    foreach(FILE_TO_COPY ${CASPARCG_RUNTIME_DEPENDENCIES_RELEASE})
		    get_filename_component(FOLDER_NAME "${FILE_TO_COPY}" NAME)
		    add_custom_command(
			    TARGET casparcg_copy_dependencies
			    POST_BUILD
			    COMMAND echo \"Copy Release ${FILE_TO_COPY}\" &&
				    ${CMAKE_COMMAND} -E copy \"${FILE_TO_COPY}\" \"${OUTPUT_FOLDER}/\" &&
				    ${CMAKE_COMMAND} -E copy \"${FILE_TO_COPY}\" \"${CMAKE_CURRENT_BINARY_DIR}/\"
		    )
	    endforeach(FILE_TO_COPY)

	    foreach(FILE_TO_COPY ${CASPARCG_RUNTIME_DEPENDENCIES_RELEASE_DIRS})
		    get_filename_component(FOLDER_NAME "${FILE_TO_COPY}" NAME)
		    add_custom_command(
			    TARGET casparcg_copy_dependencies
			    POST_BUILD
			    COMMAND echo \"Copy Release ${FILE_TO_COPY}\" &&
				    ${CMAKE_COMMAND} -E copy_directory \"${FILE_TO_COPY}\" \"${OUTPUT_FOLDER}/${FOLDER_NAME}/\" &&
				    ${CMAKE_COMMAND} -E copy_directory \"${FILE_TO_COPY}\" \"${CMAKE_CURRENT_BINARY_DIR}/${FOLDER_NAME}/\"
		    )
	    endforeach(FILE_TO_COPY)
    endif ()
else ()
	install(TARGETS casparcg)

    if ("${CASPARCG_BINARY_NAME}" STREQUAL "casparcg")
        # Looks like a 'default' build, produce a portable folder
	    install(FILES casparcg.config run.sh DESTINATION ".")
	    install(DIRECTORY template DESTINATION ".")
    else ()
        # Looks like a build for a system package manager
        install(FILES casparcg.config DESTINATION "share/${CASPARCG_BINARY_NAME}")
        install(DIRECTORY template DESTINATION "share/${CASPARCG_BINARY_NAME}")
    endif ()
endif ()
