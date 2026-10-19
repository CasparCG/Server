cmake_minimum_required (VERSION 3.28)
project (core)

set(SOURCES
		consumer/frame_consumer.cpp
		consumer/frame_consumer_registry.cpp
		consumer/output.cpp

		diagnostics/call_context.cpp
		diagnostics/osd_graph.cpp
		diagnostics/osc_graph.cpp

		frame/draw_frame.cpp
		frame/frame.cpp
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
		producer/frame_producer_registry.cpp
		producer/layer.cpp
		producer/stage.cpp

		video_channel.cpp
		video_format.cpp
)
set(HEADERS
		consumer/frame_consumer.h
		consumer/frame_consumer_registry.h
		consumer/output.h

		diagnostics/call_context.h
		diagnostics/osd_graph.h
		diagnostics/osc_graph.h

		frame/draw_frame.h
		frame/frame.h
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
		producer/frame_producer_registry.h
		producer/layer.h
		producer/stage.h

		fwd.h
		module_dependencies.h
		StdAfx.h
		video_channel.h
		video_format.h
		consumer/channel_info.h
)

casparcg_add_library(core SOURCES ${SOURCES} ${HEADERS})
target_include_directories(core PRIVATE ..)
target_precompile_headers(core PRIVATE StdAfx.h)
target_link_libraries(core PRIVATE common GLEW::glew sfml-graphics)

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
