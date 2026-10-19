cmake_minimum_required (VERSION 3.28)
project("modules")

add_subdirectory(image)
add_subdirectory(ffmpeg)
add_subdirectory(oal)
add_subdirectory(decklink)
add_subdirectory(screen)
add_subdirectory(newtek)
add_subdirectory(artnet)

if (ENABLE_PROAUDIO AND PORTAUDIO_FOUND)
	add_subdirectory(proaudio)
endif ()

if (ENABLE_HTML)
	add_subdirectory(html)
endif ()

if (MSVC)
	add_subdirectory(flash)
	add_subdirectory(bluefish)
endif()

