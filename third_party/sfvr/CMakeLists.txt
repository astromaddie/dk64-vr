cmake_minimum_required(VERSION 3.10)
project(sfvr VERSION 0.1.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(SFVR_TOP_LEVEL ON)
else()
	set(SFVR_TOP_LEVEL OFF)
endif()
option(SFVR_BUILD_TESTS "Build the sfvr tests" ${SFVR_TOP_LEVEL})

add_library(sfvr STATIC
	src/sfvr_haptics.c
	src/sfvr_perf.c
	src/sfvr_room.c
	src/sfvr_settings.c
	src/sfvr_turn.c
	src/sfvr_view.c)
target_include_directories(sfvr PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(sfvr PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT MSVC)
	target_compile_options(sfvr PRIVATE -Wall -Wextra -Werror -pedantic)
	target_link_libraries(sfvr PUBLIC m)
endif()

if(SFVR_BUILD_TESTS)
	enable_testing()
	foreach(name view turn haptics room perf settings)
		add_executable(test_${name} tests/test_${name}.c)
		target_link_libraries(test_${name} PRIVATE sfvr)
		set_target_properties(test_${name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
		if(NOT MSVC)
			target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror -pedantic)
		endif()
		add_test(NAME ${name} COMMAND test_${name})
	endforeach()
	add_executable(test_cxx tests/test_cxx.cpp)
	target_link_libraries(test_cxx PRIVATE sfvr)
	set_target_properties(test_cxx PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	if(NOT MSVC)
		target_compile_options(test_cxx PRIVATE -Wall -Wextra -Werror)
	endif()
	add_test(NAME cxx COMMAND test_cxx)
endif()
