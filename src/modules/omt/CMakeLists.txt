cmake_minimum_required(VERSION 3.28)
project(omt)

set(SOURCES
    omt.cpp
    omt.h

    consumer/omt_consumer.cpp
    consumer/omt_consumer.h

    producer/omt_producer.cpp
    producer/omt_producer.h

    util/omt_util.cpp
    util/omt_util.h

    interop/libomt.h
)

casparcg_add_module_project(omt
    SOURCES ${SOURCES}
    INIT_FUNCTION "omt::init"
)

target_include_directories(omt PRIVATE
    ..
    ${FFMPEG_INCLUDE_PATH}
)

target_link_libraries(omt PRIVATE
    core
    common
    ffmpeg
)

set_target_properties(omt PROPERTIES FOLDER modules)
source_group(sources\\consumer consumer/*)
source_group(sources\\producer producer/*)
source_group(sources\\util util/*)
source_group(sources\\interop interop/*)
source_group(sources ./*)
