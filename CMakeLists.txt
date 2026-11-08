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
	src/steam_ugc.cpp
	src/content_advert.cpp
	src/server_gate.cpp
	src/discord_ipc.cpp
	src/loading_screen.cpp
	src/loading_webview.cpp
	src/connect_watch.cpp
	src/server_rep.cpp
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
	src/doctor.cpp
	src/mouse_confine.cpp
	src/audio_watch.cpp
	src/launcher_pipe.cpp
	src/startup_hook.cpp)
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
	# data_sandbox too: the stub puts garrysmod/data right even when the pack doesn't start.
	add_library(meowy_boot SHARED src/boot/boot.cpp src/boot/boot.rc src/data_sandbox.cpp src/file_util.cpp src/pack_versions.cpp)
	target_link_libraries(meowy_boot PRIVATE bcrypt)
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
	# The player's Discord status switch, no game or Discord needed:
	#   discord_switch_smoke
	add_executable(discord_switch_smoke tests/discord_switch_smoke.cpp src/discord_ipc.cpp)
	if(MSVC)
		set_property(TARGET discord_switch_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
endif()
if(MEOWY_BUILD_TESTS)
	# Hardware profile and crash-dump faults, without the game:
	#   info_smoke [path to the GarrysMod folder]
	add_executable(info_smoke tests/info_smoke.cpp src/crash_reports.cpp src/hardware.cpp)
	if(MSVC)
		set_property(TARGET info_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
	# The per-server data folder, against a throwaway GarrysMod folder:
	#   sandbox_smoke <empty scratch folder>
	add_executable(sandbox_smoke tests/sandbox_smoke.cpp src/data_sandbox.cpp src/file_util.cpp)
	if(MSVC)
		set_property(TARGET sandbox_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
	# meowy.Notify's rules (kinds, gate, switch, focus, rate limit), no game or Windows UI:
	#   notify_smoke <empty scratch folder>
	add_executable(notify_smoke tests/notify_smoke.cpp src/notify.cpp src/file_util.cpp)
	# A real toast / balloon / taskbar badge on this PC, by hand (tests/NOTIFY_MANUAL.md):
	#   notify_manual [kind]
	add_executable(notify_manual tests/notify_manual.cpp src/notify.cpp src/notify_toast.cpp src/file_util.cpp src/meowy.rc)
	target_link_libraries(notify_manual PRIVATE runtimeobject)
	if(MSVC)
		set_property(TARGET notify_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		set_property(TARGET notify_manual PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(notify_smoke PRIVATE /W4 /utf-8)
		target_compile_options(notify_manual PRIVATE /W4 /utf-8)
	endif()
	# The signed policy (kill switch): parsing, keys, only-stricter ops key, replay, keeping:
	#   policy_smoke <scratch folder>
	add_executable(policy_smoke tests/policy_smoke.cpp src/policy.cpp src/notify_catalogue.cpp src/notify.cpp src/file_util.cpp)
	target_link_libraries(policy_smoke PRIVATE bcrypt winhttp)
	if(MSVC)
		set_property(TARGET policy_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(policy_smoke PRIVATE /W4 /utf-8)
	endif()
	# The signed updater: release documents, who takes them, and the stub's versions (promote, roll back, choose):
	#   updater_smoke <scratch folder>
	add_executable(updater_smoke tests/updater_smoke.cpp src/updater.cpp src/pack_versions.cpp src/policy.cpp src/notify_catalogue.cpp src/notify.cpp src/file_util.cpp)
	target_link_libraries(updater_smoke PRIVATE bcrypt winhttp)
	if(MSVC)
		set_property(TARGET updater_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(updater_smoke PRIVATE /W4 /utf-8)
	endif()
	# The signed notification catalogue (parser, ECDSA check, kind-list switch), no game:
	#   notify_catalogue_smoke
	add_executable(notify_catalogue_smoke tests/notify_catalogue_smoke.cpp src/notify_catalogue.cpp src/notify.cpp src/file_util.cpp)
	target_link_libraries(notify_catalogue_smoke PRIVATE bcrypt)
	if(MSVC)
		set_property(TARGET notify_catalogue_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(notify_catalogue_smoke PRIVATE /W4 /utf-8)
	endif()
	# Connection resilience's own A2S probe against one of our servers, no game:
	#   probe_smoke <a.b.c.d:port>
	add_executable(probe_smoke tests/probe_smoke.cpp src/resilience_net.cpp src/server_gate.cpp)
	target_link_libraries(probe_smoke PRIVATE ws2_32)
	if(MSVC)
		set_property(TARGET probe_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(probe_smoke PRIVATE /W4 /utf-8)
	endif()
	# PC doctor's native facts (adapters, GPU time per adapter, power), no game:
	#   doctor_smoke
	# Holding the mouse inside the game: when it clips (no real clipping):
	#   mouse_confine_smoke
	add_executable(mouse_confine_smoke tests/mouse_confine_smoke.cpp src/mouse_confine.cpp)
	if(MSVC)
		set_property(TARGET mouse_confine_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(mouse_confine_smoke PRIVATE /W4 /utf-8)
	endif()
	# Sound devices: the default output and mics as the PC doctor shows them, and the watch:
	#   audio_watch_smoke [seconds to watch]
	add_executable(audio_watch_smoke tests/audio_watch_smoke.cpp src/audio_watch.cpp)
	if(MSVC)
		set_property(TARGET audio_watch_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(audio_watch_smoke PRIVATE /W4 /utf-8)
	endif()
	add_executable(doctor_smoke tests/doctor_smoke.cpp src/doctor.cpp)
	target_link_libraries(doctor_smoke PRIVATE shell32)
	if(MSVC)
		set_property(TARGET doctor_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(doctor_smoke PRIVATE /W4 /utf-8)
	endif()
	# The start-up line in menu.lua: text rules and a file round trip, no game:
	#   startup_hook_smoke
	add_executable(startup_hook_smoke tests/startup_hook_smoke.cpp src/startup_hook.cpp src/server_gate.cpp)
	if(MSVC)
		set_property(TARGET startup_hook_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(startup_hook_smoke PRIVATE /W4 /utf-8)
	endif()
	# meowy.TimelineEvent's rules (kinds, gate, Steam missing, rate limit), no game or Steam:
	#   timeline_smoke
	add_executable(timeline_smoke tests/timeline_smoke.cpp src/timeline.cpp src/timeline_steam.cpp)
	if(MSVC)
		set_property(TARGET timeline_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(timeline_smoke PRIVATE /W4 /utf-8)
	endif()
	# Parties' rules (lobby ids, +connect_lobby, Steam missing), no game or Steam:
	#   party_smoke
	add_executable(party_smoke tests/party_smoke.cpp src/party_steam.cpp)
	if(MSVC)
		set_property(TARGET party_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(party_smoke PRIVATE /W4 /utf-8)
	endif()
	# The rejoin-after-a-crash session file and server-name cleaning, no game:
	#   session_smoke <scratch folder>
	add_executable(session_smoke tests/session_smoke.cpp src/resilience.cpp src/crash_reports.cpp src/hardware.cpp src/server_gate.cpp src/file_util.cpp)
	if(MSVC)
		set_property(TARGET session_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(session_smoke PRIVATE /W4 /utf-8)
	endif()
	# The fast client Lua lookup: same answers as the game's scan on a fake string
	# table, and the patch site found in the installed client.dll, no game running:
	#   lua_lookup_smoke [GarrysMod folder] [file to write the stub to]
	add_executable(lua_lookup_smoke tests/lua_lookup_smoke.cpp src/lua_lookup.cpp)
	if(MSVC)
		set_property(TARGET lua_lookup_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(lua_lookup_smoke PRIVATE /W4 /utf-8)
	endif()
	# Join pre-warm: the per-server lists, recording, and a warm run on scratch files:
	#   prewarm_smoke <scratch folder>
	add_executable(prewarm_smoke tests/prewarm_smoke.cpp src/prewarm.cpp src/file_util.cpp)
	if(MSVC)
		set_property(TARGET prewarm_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(prewarm_smoke PRIVATE /W4 /utf-8)
	endif()
	# The LZMA pak cache: hits, misses, the memory cap, with a stand-in decoder:
	#   pak_cache_smoke
	add_executable(pak_cache_smoke tests/pak_cache_smoke.cpp src/pak_cache.cpp)
	if(MSVC)
		set_property(TARGET pak_cache_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(pak_cache_smoke PRIVATE /W4 /utf-8)
	endif()
	# Copies of config.cfg: one per change, the newest 10, written back for exec:
	#   config_backup_smoke <scratch folder>
	add_executable(config_backup_smoke tests/config_backup_smoke.cpp src/config_backup.cpp src/file_util.cpp)
	if(MSVC)
		set_property(TARGET config_backup_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(config_backup_smoke PRIVATE /W4 /utf-8)
	endif()
	# The fast file.Find on "LUA": the walk visits every entry the game's matcher
	# accepts, in order, on a fake table; and the loop found in the installed client.dll:
	#   lua_find_smoke [GarrysMod folder] [file to write the stubs to]
	add_executable(lua_find_smoke tests/lua_find_smoke.cpp src/lua_find.cpp)
	if(MSVC)
		set_property(TARGET lua_find_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(lua_find_smoke PRIVATE /W4 /utf-8)
	endif()
	# The console capture's lines, no game (console_capture_smoke).
	add_executable(console_capture_smoke tests/console_capture_smoke.cpp src/console_capture.cpp)
	if(MSVC)
		set_property(TARGET console_capture_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(console_capture_smoke PRIVATE /W4 /utf-8)
	endif()
	# Server lists from Steam: the filters passed on, no game (server_query_smoke).
	add_executable(server_query_smoke tests/server_query_smoke.cpp src/server_query.cpp)
	if(MSVC)
		set_property(TARGET server_query_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(server_query_smoke PRIVATE /W4 /utf-8)
	endif()
	# Content platform adverts (the meowy_content rule in A2S_RULES), no game:
	#   content_advert_smoke [a.b.c.d:port to ask for real]
	add_executable(content_advert_smoke tests/content_advert_smoke.cpp src/content_advert.cpp src/server_rep.cpp)
	target_link_libraries(content_advert_smoke PRIVATE winhttp ws2_32)
	target_compile_definitions(content_advert_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)   # server_rep.cpp's _wfopen
	if(MSVC)
		set_property(TARGET content_advert_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(content_advert_smoke PRIVATE /W4 /utf-8)
	endif()
	# The Meowy Launcher's pipe, the pack's end: what it takes and sends, no game:
	#   launcher_pipe_smoke
	add_executable(launcher_pipe_smoke tests/launcher_pipe_smoke.cpp src/launcher_pipe.cpp src/server_gate.cpp src/server_rep.cpp)
	target_link_libraries(launcher_pipe_smoke PRIVATE winhttp ws2_32 advapi32)
	target_compile_definitions(launcher_pipe_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)   # server_rep.cpp's _wfopen
	if(MSVC)
		set_property(TARGET launcher_pipe_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(launcher_pipe_smoke PRIVATE /W4 /utf-8)
	endif()
	# The missing-file cache's rules against real folders, no game:
	#   fs_cache_smoke <scratch folder>
	add_executable(fs_cache_smoke tests/fs_cache_smoke.cpp src/fs_cache.cpp src/prewarm.cpp src/file_util.cpp)
	if(MSVC)
		set_property(TARGET fs_cache_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(fs_cache_smoke PRIVATE /W4 /utf-8)
	endif()
	# The font pass's gdi32 loads, on this program's own imports and a real font file:
	#   font_loads_smoke <scratch folder>
	add_executable(font_loads_smoke tests/font_loads_smoke.cpp src/font_loads.cpp)
	target_link_libraries(font_loads_smoke PRIVATE gdi32)
	if(MSVC)
		set_property(TARGET font_loads_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		target_compile_options(font_loads_smoke PRIVATE /W4 /utf-8)
	endif()
endif()
