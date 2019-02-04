project (terrac)
cmake_minimum_required (VERSION 3.2)

add_subdirectory (ext/xopt)

include_directories (
	/usr/local/include # for whatever reason, cmake doesn't include this as a default, nor does clang
	ext/xopt
	ext/filesystem)

link_directories (
	/usr/local/lib) # same thing here.

# Compilation flags
if (("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang") OR ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU"))
	set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -pedantic -std=c++11 -Wno-invalid-offsetof")
	set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g3 -O0")
	set (CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Ofast")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	# TODO MSVC flags
	message (FATAL_ERROR "MSVC not yet supported - add some build flags!")
endif ()

add_executable (terrac main.cc)
target_link_libraries (terrac
	terra
	z
	curses
	xopt)
