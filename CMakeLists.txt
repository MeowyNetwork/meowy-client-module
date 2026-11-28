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
	src/lua_lookup.cpp
	src/lua_find.cpp
	src/font_loads.cpp
	src/fs_cache.cpp
	src/prewarm.cpp
	src/pak_cache.cpp
	src/console_capture.cpp
	src/server_query.cpp
	src/server_index.cpp
	src/net_http.cpp
	src/steam_ugc.cpp
	src/content_advert.cpp
	src/server_gate.cpp
	src/discord_ipc.cpp
	src/loading_screen.cpp
	src/loading_webview.cpp
	src/connect_watch.cpp
	src/server_rep.cpp
	src/steam_ticket.cpp
	src/data_sandbox.cpp
	src/file_util.cpp
	src/notify.cpp
	src/notify_catalogue.cpp
	src/policy.cpp
	src/updater.cpp
	src/pack_versions.cpp
	src/notify_toast.cpp
	src/timeline.cpp
	src/timeline_steam.cpp
	src/party_steam.cpp
	src/resilience.cpp
	src/resilience_net.cpp
	src/config_backup.cpp
	src/apps_entry.cpp
	src/doctor.cpp
	src/mouse_confine.cpp
	src/audio_watch.cpp
	src/launcher_pipe.cpp
	src/startup_hook.cpp
	src/multicore.cpp)
target_include_directories(meowy PRIVATE ${gmod_module_base_SOURCE_DIR}/include)
if(WIN32)
	target_link_libraries(meowy PRIVATE msimg32 shlwapi winhttp ws2_32 shell32 bcrypt)   # GradientFill and file streams for the loading screen; HTTPS for the server list
	# Notifications: WinRT toasts (RoGetActivationFactory, HSTRINGs) and the
	# bundled icons (src/meowy.rc).
	target_sources(meowy PRIVATE src/meowy.rc)
	target_link_libraries(meowy PRIVATE runtimeobject)

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
	target_compile_options(meowy PRIVATE /W4 /utf-8 /MP)
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
	# data_sandbox too: the stub puts garrysmod/data right even when the pack doesn't start.
	add_library(meowy_boot SHARED src/boot/boot.cpp src/boot/boot.rc src/data_sandbox.cpp src/file_util.cpp src/pack_versions.cpp)
	target_link_libraries(meowy_boot PRIVATE bcrypt)
	target_include_directories(meowy_boot PRIVATE ${gmod_module_base_SOURCE_DIR}/include)
	if(MSVC)
		set_property(TARGET meowy_boot PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(meowy_boot PRIVATE /W4 /utf-8 /MP)
	endif()
	set_target_properties(meowy_boot PROPERTIES
		PREFIX ""
		OUTPUT_NAME "gmsv_meowy_boot_${MEOWY_SUFFIX}"
		SUFFIX ".dll")
endif()

# The tests (tests/*_smoke.cpp), each a small program that needs no game:
#   cmake -S . -B buildtest -A x64 -DMEOWY_BUILD_TESTS=ON
#   cmake --build buildtest --config Release --parallel
#   pwsh tests/run_smokes.ps1 -Build buildtest
# One build tree gives the DLLs and every test, so CI configures once per
# architecture. The pack's sources the tests use are compiled once, into
# meowy_testlib; a test links what it needs from it. The DLLs above are built
# from their own sources exactly as before: nothing shipped comes from the
# test library.
option(MEOWY_BUILD_TESTS "Build the tests in tests/ (the *_smoke programs)" OFF)
if(MEOWY_BUILD_TESTS AND WIN32)
	add_library(meowy_testlib STATIC
		src/apps_entry.cpp
		src/audio_watch.cpp
		src/config_backup.cpp
		src/console_capture.cpp
		src/content_advert.cpp
		src/crash_reports.cpp
		src/data_sandbox.cpp
		src/discord_ipc.cpp
		src/doctor.cpp
		src/file_util.cpp
		src/font_loads.cpp
		src/fs_cache.cpp
		src/hardware.cpp
		src/launcher_pipe.cpp
		src/lua_find.cpp
		src/lua_lookup.cpp
		src/mouse_confine.cpp
		src/multicore.cpp
		src/net_http.cpp
		src/notify.cpp
		src/notify_catalogue.cpp
		src/pack_versions.cpp
		src/pak_cache.cpp
		src/party_steam.cpp
		src/policy.cpp
		src/prewarm.cpp
		src/resilience.cpp
		src/resilience_net.cpp
		src/server_gate.cpp
		src/server_index.cpp
		src/server_query.cpp
		src/server_rep.cpp
		src/startup_hook.cpp
		src/steam_ticket.cpp
		src/timeline.cpp
		src/timeline_steam.cpp
		src/updater.cpp)
	target_compile_definitions(meowy_testlib PRIVATE _CRT_SECURE_NO_WARNINGS)   # server_rep.cpp's _wfopen
	target_link_libraries(meowy_testlib PUBLIC bcrypt winhttp ws2_32 advapi32 gdi32 shell32)
	if(MSVC)
		set_property(TARGET meowy_testlib PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(meowy_testlib PRIVATE /W4 /utf-8 /MP)
	endif()

	# meowy_smoke(<name> [SCRATCH] [MANUAL] [SOURCES <file>...] [LIBS <lib>...])
	#   tests/<name>.cpp, linked with meowy_testlib. SCRATCH: it takes an empty
	#   folder as its argument. MANUAL: built, never run by tests/run_smokes.ps1
	#   (it needs a device, an app or a person). SOURCES: files of its own that
	#   aren't in the library.
	set(MEOWY_SMOKES "")
	function(meowy_smoke name)
		cmake_parse_arguments(S "SCRATCH;MANUAL" "" "SOURCES;LIBS" ${ARGN})
		add_executable(${name} tests/${name}.cpp ${S_SOURCES})
		target_link_libraries(${name} PRIVATE meowy_testlib ${S_LIBS})
		if(MSVC)
			set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
			target_compile_options(${name} PRIVATE /W4 /utf-8)
		endif()
		if(NOT S_MANUAL)
			if(S_SCRATCH)
				set(kind scratch)
			else()
				set(kind plain)
			endif()
			set(MEOWY_SMOKES "${MEOWY_SMOKES}${name} ${kind}\n" PARENT_SCOPE)
		endif()
	endfunction()

	# By hand (not run in CI): they need the Discord app, sound devices, a GPU, or eyes.
	meowy_smoke(ipc_smoke MANUAL)                 # ipc_smoke <client id> [seconds]
	meowy_smoke(audio_watch_smoke MANUAL)         # audio_watch_smoke [seconds to watch]
	meowy_smoke(doctor_smoke MANUAL)              # PC doctor's native facts
	meowy_smoke(notify_manual MANUAL              # a real toast (tests/NOTIFY_MANUAL.md)
		SOURCES src/notify_toast.cpp src/meowy.rc LIBS runtimeobject)

	meowy_smoke(apps_entry_smoke)                 # the installed-apps entry's version (scratch HKCU key)
	meowy_smoke(config_backup_smoke SCRATCH)      # copies of config.cfg: one per change, the newest 10
	meowy_smoke(console_capture_smoke)            # the console capture's lines
	meowy_smoke(content_advert_smoke)             # the meowy_content rule in A2S_RULES [a.b.c.d:port to ask for real]
	meowy_smoke(discord_switch_smoke)             # the player's Discord status switch
	meowy_smoke(font_loads_smoke SCRATCH)         # the font pass's gdi32 loads, on its own imports and a real font
	meowy_smoke(fs_cache_smoke SCRATCH)           # the missing-file cache's rules against real folders
	meowy_smoke(info_smoke)                       # hardware profile and crash-dump faults [GarrysMod folder]
	meowy_smoke(launcher_pipe_smoke)              # the Meowy Launcher's pipe, the pack's end
	meowy_smoke(lua_find_smoke)                   # fast file.Find on a fake table [GarrysMod folder] [stub file]
	meowy_smoke(lua_lookup_smoke)                 # fast Lua lookup on a fake table [GarrysMod folder] [stub file]
	meowy_smoke(mouse_confine_smoke)              # when the mouse is held inside the game (no real clipping)
	meowy_smoke(multicore_smoke SCRATCH)          # multicore's managed flag and the crash guard's write at exit
	meowy_smoke(notify_catalogue_smoke)           # the signed notification catalogue
	meowy_smoke(notify_smoke SCRATCH)             # meowy.Notify's rules
	meowy_smoke(pak_cache_smoke)                  # the LZMA pak cache, with a stand-in decoder
	meowy_smoke(party_smoke)                      # parties' rules, no Steam
	meowy_smoke(policy_smoke SCRATCH)             # the signed policy: parsing, keys, only-stricter ops key, keeping
	meowy_smoke(prewarm_smoke SCRATCH)            # join pre-warm: lists, recording, a warm run on scratch files
	meowy_smoke(probe_smoke)                      # connection resilience's A2S probe [a.b.c.d:port]
	meowy_smoke(sandbox_smoke SCRATCH)            # the per-server data folder, against a throwaway GarrysMod folder
	meowy_smoke(server_query_smoke)               # server lists from Steam: the filters passed on
	meowy_smoke(session_smoke SCRATCH)            # the rejoin-after-a-crash session file
	meowy_smoke(startup_hook_smoke)               # the start-up line in menu.lua
	meowy_smoke(steam_ticket_smoke)               # the report's Steam ticket [--live <GarrysMod folder>]
	meowy_smoke(timeline_smoke)                   # meowy.TimelineEvent's rules, no Steam
	meowy_smoke(updater_smoke SCRATCH)            # the signed updater: documents, who takes them, the stub's versions

	# What tests/run_smokes.ps1 runs: "<name> scratch|plain" per line.
	file(WRITE "${CMAKE_BINARY_DIR}/smokes.txt" "${MEOWY_SMOKES}")
endif()
