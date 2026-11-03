cmake_minimum_required(VERSION 3.20)
project(meowy-client-module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Facepunch's official module headers, pinned to a commit so builds are
# reproducible. SOURCE_SUBDIR points at a folder that doesn't exist so
# MakeAvailable only downloads (we don't want its example projects).
include(FetchContent)
FetchContent_Declare(gmod_module_base
	GIT_REPOSITORY https://github.com/Facepunch/gmod-module-base.git
	GIT_TAG e0156e5f96cd0bad8eed738f0d20e2c9a14b2a0d  # development: has the 64-bit lua_State layout (master does not)
	SOURCE_SUBDIR do-not-build)
FetchContent_MakeAvailable(gmod_module_base)

add_library(meowy SHARED
	src/module.cpp
	src/crash_reports.cpp
	src/discord_ipc.cpp)
target_include_directories(meowy PRIVATE ${gmod_module_base_SOURCE_DIR}/include)


if(MSVC)
	# Static CRT: players don't need a VC++ redistributable installed.
	set_property(TARGET meowy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	target_compile_options(meowy PRIVATE /W4 /utf-8)
endif()

# GMod looks for gmcl_<name>_<platform>.dll in garrysmod/lua/bin.
if(WIN32)
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(MEOWY_SUFFIX win64)
	else()
		set(MEOWY_SUFFIX win32)
	endif()
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(MEOWY_SUFFIX linux64)
else()
	set(MEOWY_SUFFIX linux)
endif()
set_target_properties(meowy PROPERTIES
	PREFIX ""
	OUTPUT_NAME "gmcl_meowy_${MEOWY_SUFFIX}"
	SUFFIX ".dll")

# Standalone check of the Discord IPC layer, no game needed:
#   ipc_smoke <client id> [seconds]
option(MEOWY_BUILD_TESTS "Build tests/ipc_smoke" OFF)
if(MEOWY_BUILD_TESTS)
	add_executable(ipc_smoke tests/ipc_smoke.cpp src/discord_ipc.cpp)
	if(MSVC)
		set_property(TARGET ipc_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
endif()
