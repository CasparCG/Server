cmake_minimum_required (VERSION 2.6)
project (core)

set(SOURCES
		consumer/frame_consumer.cpp
		consumer/output.cpp

		diagnostics/call_context.cpp
		diagnostics/osd_graph.cpp

		frame/draw_frame.cpp
		frame/frame.cpp
		frame/frame_timecode.cpp
		frame/frame_transform.cpp
		frame/geometry.cpp

		mixer/audio/audio_mixer.cpp
		mixer/image/blend_modes.cpp
		mixer/mixer.cpp

		producer/color/color_producer.cpp
		producer/separated/separated_producer.cpp
		producer/transition/transition_producer.cpp
		producer/transition/sting_producer.cpp
		producer/route/route_producer.cpp

		producer/cg_proxy.cpp
		producer/frame_producer.cpp
		producer/layer.cpp
		producer/stage.cpp

		StdAfx.cpp
		video_channel.cpp
		video_format.cpp
		channel_timecode.cpp
)
set(HEADERS
		consumer/frame_consumer.h
		consumer/output.h

		diagnostics/call_context.h
		diagnostics/osd_graph.h

		frame/draw_frame.h
		frame/frame.h
		frame/frame_timecode.h
		frame/frame_factory.h
		frame/frame_transform.h
		frame/frame_visitor.h
		frame/geometry.h
		frame/pixel_format.h

		mixer/audio/audio_mixer.h

		mixer/image/blend_modes.h
		mixer/image/image_mixer.h

		mixer/mixer.h

		monitor/monitor.h

		producer/color/color_producer.h
		producer/separated/separated_producer.h
		producer/transition/transition_producer.h
		producer/transition/sting_producer.h
		producer/route/route_producer.h

		producer/cg_proxy.h
		producer/frame_producer.h
		producer/timecode_source.h
		producer/layer.h
		producer/stage.h

		fwd.h
		module_dependencies.h
		StdAfx.h
		video_channel.h
		video_format.h
		channel_timecode.h
)

add_library(core ${SOURCES} ${HEADERS})
add_precompiled_header(core StdAfx.h FORCEINCLUDE)
configure_file("${PROJECT_SOURCE_DIR}/packages.config" "${CMAKE_CURRENT_BINARY_DIR}/packages.config")

include_directories(..)
include_directories(${BOOST_INCLUDE_PATH})
include_directories(${TBB_INCLUDE_PATH})
include_directories(${SFML_INCLUDE_PATH})
include_directories(${FREETYPE_INCLUDE_PATH})
include_directories(${GLEW_INCLUDE_PATH})

source_group(sources ./*)
source_group(sources\\consumer consumer/*)
source_group(sources\\diagnostics diagnostics/*)
source_group(sources\\producer producer/*)
source_group(sources\\monitor monitor/*)
source_group(sources\\frame frame/*)
source_group(sources\\mixer mixer/*)
source_group(sources\\mixer\\audio mixer/audio/*)
source_group(sources\\mixer\\image mixer/image/*)
source_group(sources\\producer\\color producer/color/*)
source_group(sources\\producer\\route producer/route/*)
source_group(sources\\producer\\transition producer/transition/*)
source_group(sources\\producer\\separated producer/separated/*)

if (MSVC)
	target_link_libraries(core
			common
	)
else()
	target_link_libraries(core
			common
	)
endif()
