cmake_minimum_required(VERSION 3.16)
project(inmarsat-rx C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")
option(NATIVE "Optimise for this CPU (-march=native)" ON)
if(NATIVE)
	add_compile_options(-march=native)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBACARS libacars-2)
pkg_check_modules(LIBAIRSPY libairspy)
pkg_check_modules(LIBHACKRF libhackrf)

# The live page is kept as HTML and embedded at build time.
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/web_page.c
	COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/web_page.html
		-DOUT=${CMAKE_CURRENT_BINARY_DIR}/web_page.c -DNAME=WEB_PAGE -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
	DEPENDS src/web_page.html cmake/embed.cmake)

add_executable(inmarsat-rx
	src/main.c src/channel.c src/dsp.c src/frontend.c src/psk.c src/msk.c src/fec.c
	src/aero.c src/stdc.c src/apps.c src/out.c src/input_file.c src/input_live.c
	src/plans.c src/events.c src/archive.c src/web.c src/scan.c src/cluster.c ${CMAKE_CURRENT_BINARY_DIR}/web_page.c)
target_link_libraries(inmarsat-rx m Threads::Threads)

if(LIBACARS_FOUND)
	target_compile_definitions(inmarsat-rx PRIVATE HAVE_LIBACARS)
	target_include_directories(inmarsat-rx PRIVATE ${LIBACARS_INCLUDE_DIRS})
	target_link_directories(inmarsat-rx PRIVATE ${LIBACARS_LIBRARY_DIRS})
	target_link_libraries(inmarsat-rx ${LIBACARS_LIBRARIES})
	message(STATUS "libacars-2 found: CPDLC/ADS-C decoding on")
else()
	message(STATUS "libacars-2 not found: building without CPDLC/ADS-C decoding")
endif()

foreach(lib AIRSPY HACKRF)
	if(LIB${lib}_FOUND)
		target_compile_definitions(inmarsat-rx PRIVATE HAVE_${lib})
		target_include_directories(inmarsat-rx PRIVATE ${LIB${lib}_INCLUDE_DIRS})
		target_link_directories(inmarsat-rx PRIVATE ${LIB${lib}_LIBRARY_DIRS})
		target_link_libraries(inmarsat-rx ${LIB${lib}_LIBRARIES})
		message(STATUS "lib${lib} found: live input on")
	else()
		message(STATUS "lib${lib} not found: building without it")
	endif()
endforeach()

install(TARGETS inmarsat-rx RUNTIME DESTINATION bin)
