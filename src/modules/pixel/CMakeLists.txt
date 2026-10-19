cmake_minimum_required(VERSION 3.28)
project(pixel)

set(SOURCES
    consumer/artdmx_sink.h
    consumer/color_grader.h
    consumer/pixel_consumer.cpp
    consumer/pixel_consumer.h
    consumer/types.h

    pixel.cpp
    pixel.h
)

casparcg_add_module_project(pixel
    SOURCES ${SOURCES}
    INIT_FUNCTION "pixel::init"
)

set_target_properties(pixel PROPERTIES FOLDER modules)
source_group(sources\\consumer consumer/*)
source_group(sources ./*)
