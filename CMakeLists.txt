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
	src/hardware.cpp
	src/engine_patches.cpp
	src/server_gate.cpp
	src/discord_ipc.cpp
	src/loading_screen.cpp
	src/loading_webview.cpp)
target_include_directories(meowy PRIVATE ${gmod_module_base_SOURCE_DIR}/include)
if(WIN32)
	target_link_libraries(meowy PRIVATE msimg32 shlwapi)   # GradientFill and file streams, for the loading screen

	# Microsoft's WebView2 SDK (headers and the static loader, so there's no
	# extra DLL to ship), pinned by hash: the loading screen's web page.
	FetchContent_Declare(webview2
		URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2903.40
		URL_HASH SHA256=ef128016dd1e51c59178c827ed5b8aa3322c57afa8675d930f8109505542ad74
		DOWNLOAD_NAME webview2-1.0.2903.40.zip
		DOWNLOAD_EXTRACT_TIMESTAMP TRUE
		SOURCE_SUBDIR do-not-build)
	FetchContent_MakeAvailable(webview2)
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(MEOWY_WV2_ARCH x64)
	else()
		set(MEOWY_WV2_ARCH x86)
	endif()
	target_include_directories(meowy PRIVATE ${webview2_SOURCE_DIR}/build/native/include)
	target_link_libraries(meowy PRIVATE ${webview2_SOURCE_DIR}/build/native/${MEOWY_WV2_ARCH}/WebView2LoaderStatic.lib)
endif()


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

# Start-up stub: GMod's menu state only loads gmsv_ modules, so this small
# DLL is what lua/includes/menu.lua requires; it loads gmcl_meowy (src/boot).
if(WIN32)
	add_library(meowy_boot SHARED src/boot/boot.cpp)
	target_include_directories(meowy_boot PRIVATE ${gmod_module_base_SOURCE_DIR}/include)
	if(MSVC)
		set_property(TARGET meowy_boot PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(meowy_boot PRIVATE /W4 /utf-8)
	endif()
	set_target_properties(meowy_boot PROPERTIES
		PREFIX ""
		OUTPUT_NAME "gmsv_meowy_boot_${MEOWY_SUFFIX}"
		SUFFIX ".dll")
endif()

# Standalone check of the Discord IPC layer, no game needed:
#   ipc_smoke <client id> [seconds]
option(MEOWY_BUILD_TESTS "Build tests/ipc_smoke" OFF)
if(MEOWY_BUILD_TESTS)
	add_executable(ipc_smoke tests/ipc_smoke.cpp src/discord_ipc.cpp)
	if(MSVC)
		set_property(TARGET ipc_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
endif()
if(MEOWY_BUILD_TESTS)
	# Hardware profile and crash-dump faults, without the game:
	#   info_smoke [path to the GarrysMod folder]
	add_executable(info_smoke tests/info_smoke.cpp src/crash_reports.cpp src/hardware.cpp)
	if(MSVC)
		set_property(TARGET info_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
endif()
