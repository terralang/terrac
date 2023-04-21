project (terrac)
cmake_minimum_required (VERSION 3.2)

add_subdirectory (ext/xopt)

if (NOT DEFINED TERRA_PREFIX)
	set (TERRA_PREFIX /usr/local)
endif ()

# Makes LuaJIT happy on MacOS
add_executable (terrac main.cc)
target_link_libraries (terrac PRIVATE
	terra
	z
	curses
	xopt)

target_compile_features (terrac PRIVATE cxx_std_17)

target_include_directories (terrac PRIVATE
	"${TERRA_PREFIX}/include"
	ext/xopt)

target_link_directories (terrac PRIVATE "${TERRA_PREFIX}/lib")

if (("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang") OR ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU"))
	target_compile_options (terrac PRIVATE
		-Wall -Wextra -Werror -pedantic -Wno-invalid-offsetof
		$<$<CONFIG:Debug>:-g3 -O0>
		$<$<CONFIG:Release>:-O3 -g0>)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	# TODO MSVC flags
endif ()

if (APPLE)
	set_target_properties(terrac PROPERTIES
		# Makes LuaJIT happy (otherwise luaL_newstate returns NULL). Has to be
		# set as a property otherwise target_link_libraries thinks the arguments
		# are libraries.
		LINK_FLAGS "-pagezero_size 10000 -image_base 100000000")
elseif (UNIX)
	target_link_libraries (terrac PRIVATE dl pthread)
endif ()
