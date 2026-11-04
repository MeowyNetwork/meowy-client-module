cmake_minimum_required(VERSION 3.20)
project(meowy-observer LANGUAGES CXX)

# The observer's module (src/observer.cpp), on its own so it builds on Linux,
# where the observer runs, without the Windows-only parts of the Client Pack.
#   cmake -S observer -B build-observer && cmake --build build-observer --config Release

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(gmod_module_base
	GIT_REPOSITORY https://github.com/Facepunch/gmod-module-base.git
	GIT_TAG e0156e5f96cd0bad8eed738f0d20e2c9a14b2a0d  # same pin as the Client Pack
	SOURCE_SUBDIR do-not-build)
FetchContent_MakeAvailable(gmod_module_base)

# Both Lua scripts compiled in as raw strings: nothing is read from garrysmod/data.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/lua/menu.lua MEOWY_MENU_LUA)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/lua/sampler.lua MEOWY_SAMPLER_LUA)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/observer_lua.h
	"#pragma once\n// Generated from observer/lua by CMake; edit the .lua files.\n"
	"static const char* const kObserverMenuLua = R\"meowylua(${MEOWY_MENU_LUA})meowylua\";\n"
	"static const char* const kSamplerLua = R\"meowylua(${MEOWY_SAMPLER_LUA})meowylua\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS lua/menu.lua lua/sampler.lua)

add_library(meowy_observer SHARED src/observer.cpp)
target_include_directories(meowy_observer PRIVATE
	${gmod_module_base_SOURCE_DIR}/include
	${CMAKE_CURRENT_BINARY_DIR}/generated)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_CXX_FLAGS MATCHES "-m32")
	set(ARCH_BITS 64)
else()
	set(ARCH_BITS 32)
endif()
if(WIN32)
	set(MEOWY_OBSERVER_SUFFIX "win${ARCH_BITS}")
	if(MSVC)
		set_property(TARGET meowy_observer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(meowy_observer PRIVATE /W4 /utf-8)
		target_compile_definitions(meowy_observer PRIVATE _CRT_SECURE_NO_WARNINGS)
	endif()
else()
	set(MEOWY_OBSERVER_SUFFIX "linux${ARCH_BITS}")
	target_compile_options(meowy_observer PRIVATE -Wall -O2 -fvisibility=hidden)
	target_link_libraries(meowy_observer PRIVATE dl)
endif()

# GMod loads binary modules as gmsv_<name>_<platform>.dll on every OS.
set_target_properties(meowy_observer PROPERTIES
	PREFIX ""
	OUTPUT_NAME "gmsv_meowy_observer_${MEOWY_OBSERVER_SUFFIX}"
	SUFFIX ".dll")
