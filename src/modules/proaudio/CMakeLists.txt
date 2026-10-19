cmake_minimum_required (VERSION 3.28)
project (proaudio)

set(SOURCES
		consumer/proaudio_consumer.cpp
		consumer/proaudio_consumer.h

		util/device_util.cpp
		util/device_util.h
		util/ring_buffer.h
		proaudio.cpp
		proaudio.h
)

casparcg_add_module_project(proaudio
	SOURCES ${SOURCES}
	INIT_FUNCTION "proaudio::init"
)
target_link_libraries(proaudio PRIVATE PortAudio::PortAudio)

set_target_properties(proaudio PROPERTIES FOLDER modules)
source_group(sources\\consumer consumer/*)
source_group(sources\\util util/*)
source_group(sources ./*)
