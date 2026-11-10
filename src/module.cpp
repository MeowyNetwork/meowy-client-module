// gmcl_meowy: optional client module for Meowy Network.
//
// Lua API (global table `meowy`, created by require("meowy")):
//   meowy.VERSION                     "1.3.0"
//   meowy.DiscordStart(clientId)      connect to the local Discord app (1.7.2+:
//                                     not while the player has Discord status
//                                     off in Meowy settings; it's remembered)
//   meowy.DiscordSetActivity(tbl)     set Rich Presence (fields below)
//   meowy.DiscordClear()              remove it
//   meowy.DiscordState()              "unsupported" | "stopped" | "connecting" | "ready"
//   meowy.DiscordLastError()          Discord's last error reply (raw JSON), or nil
//   meowy.CrashReports(since, max)    the game's crash reports written after
//                                     `since` (unix time): { { time, lua, fault }, ... },
//                                     newest first; `lua` is only the report's
//                                     Lua stack trace section (max 4 KB); `fault`
//                                     is "module.dll+0xOFFSET (0xCODE)" from the
//                                     dump beside it, or "" (1.2+); the same as
//                                     fault_module / fault_offset / fault_code
//   meowy.Hardware()                  { cpu, threads, ram_mb, gpu, vram_mb, os } (1.2+)
//   meowy.SkipMaterialReload()        true | false, reason (1.3+): stop
//                                     GMod reloading every material at the end of
//                                     client Lua start-up; call from client autorun
//   meowy.RestoreMaterialReload()     undo it
//   meowy.MaterialReloadState()       "patched" | "not patched" | "unsupported"
//   meowy.ServerAllowed()             true | false, reason (1.2.1+)
//   meowy.LoadingDone()               the player has spawned and the game runs
//                                     smoothly: fade out our loading screen (1.4+)
//   meowy.Notify(kind)                true | false, reason (1.5+): a Windows
//                                     notification while the game isn't in front.
//                                     `kind` is one of notify.cpp's fixed list; the
//                                     text shown is the pack's own for that kind
//                                     (anything else passed is ignored). Reasons:
//                                     "only works on Meowy Network servers",
//                                     "unknown kind", "disabled", "focused",
//                                     "rate limited". See notify.h, notify_toast.h
//   meowy.SetNotifyEnabled(bool)      true | false, reason: the player's switch for
//                                     all of them (on unless turned off; kept in
//                                     %LOCALAPPDATA%\MeowyClientPack\notify.txt)
//   meowy.NotifyEnabled()             true | false, or nil, reason
//   meowy.NotifyCatalogue(body, sig)  true, version | false, reason (1.6+): our
//                                     signed list of kinds and their words, so
//                                     kinds added after this release still toast.
//                                     Taken only if the signature is ours (key
//                                     compiled in), it parses, and it isn't older
//                                     than the one already taken; kept in
//                                     %LOCALAPPDATA%\MeowyClientPack and checked
//                                     again when read back. Reasons: "only works
//                                     on Meowy Network servers", "bad signature",
//                                     "bad catalogue: ...", "older catalogue".
//                                     See notify_catalogue.h
//   meowy.TimelineEvent(kind)         true | false, reason: a marker on Steam's
//                                     game-recording timeline ("Arrested"), whether
//                                     or not the game is in front. `kind` is one of
//                                     timeline.cpp's fixed list; title, description
//                                     and icon are the pack's. Reasons: "only works
//                                     on Meowy Network servers", "unknown kind",
//                                     "timeline unavailable", "rate limited". See
//                                     timeline.h, timeline_steam.h
//   meowy.ExpectRestart(seconds)      true, seconds | false, reason (1.6+): the
//                                     server restarts within `seconds` (clamped
//                                     to 30..600); if the connection ends before
//                                     then, players who opted in to staying
//                                     connected are rejoined once it's back.
//                                     See resilience.h
//
// Since 1.2.1 everything except VERSION, DiscordState, DiscordClear,
// RestoreMaterialReload and MaterialReloadState only
// works while the game is connected to a Meowy Network server, checked against
// the engine's own connection (server_gate.h). Anywhere else the calls return
// false (CrashReports: an empty table; Hardware: nil) and a reason.
//
// DiscordSetActivity fields, all optional:
//   details, state       strings, shown on two lines
//   start                unix time; Discord shows "elapsed"
//   large_image, large_text
//   party_size, party_max
//   buttons              up to 2 { label = "...", url = "https://..." } (https only)
//
// The module only talks to Discord's local IPC pipe (and, for TimelineEvent,
// to Steam through the steam_api the game already loaded), reads the game's own
// crashes/ folder and asks Windows for the CPU/GPU/RAM description. It makes
// no network connections. It writes only in %LOCALAPPDATA%\MeowyClientPack
// (not garrysmod/data, which servers' Lua can read): the start-up stub's
// boot.lock, the loading screen's join_times.txt, last_join.txt and
// css_scale.txt (loading_screen.h), notify.txt and notify_icon.png (the
// notifications' switch and the toast's logo), resilience.txt and on_server.txt
// (connection resilience, resilience.h), and WebView2's profile. The one change it can make to
// the game is SkipMaterialReload, in memory only, and only when Lua asks.

#include "GarrysMod/Lua/Interface.h"
#include "crash_reports.h"
#include "discord_ipc.h"
#include "engine_patches.h"
#include "lua_find.h"
#include "lua_lookup.h"
#include "font_loads.h"
#include "fs_cache.h"
#include "file_util.h"
#include "hardware.h"
#include "connect_watch.h"
#include "loading_screen.h"
#include "server_gate.h"
#include "data_sandbox.h"
#include "server_rep.h"
#include "steam_ticket.h"
#include "launcher_pipe.h"
#include "notify.h"
#include "notify_catalogue.h"
#include "notify_toast.h"
#include "timeline.h"
#include "timeline_steam.h"
#include "prewarm.h"
#include "pak_cache.h"
#include "policy.h"
#include "version.h"
#include "updater.h"
#include "mouse_confine.h"
#include "config_backup.h"
#include "console_capture.h"
#include "server_query.h"
#include "server_index.h"
#include "party_steam.h"
#include "steam_ugc.h"
#include "content_advert.h"
#include "resilience.h"
#include "doctor.h"
#include "audio_watch.h"
#include "startup_hook.h"
#include "pack_versions.h"
#include "apps_entry.h"
#include "resource.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <algorithm>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace GarrysMod::Lua;

namespace
{
	const char* const VERSION = MEOWY_VERSION;   // version.h, also the DLL's version resource

	// Discord's own limits; anything longer makes it reject the whole activity.
	constexpr size_t MAX_TEXT = 128;
	constexpr size_t MAX_LABEL = 32;
	constexpr size_t MAX_URL = 512;

	// Cuts at a UTF-8 character boundary.
	std::string Truncate(const char* s, size_t len, size_t max)
	{
		if (len <= max)
			return std::string(s, len);
		size_t cut = max;
		while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
			--cut;
		return std::string(s, cut);
	}

	void AppendJsonString(std::string& out, const std::string& s)
	{
		out += '"';
		for (unsigned char c : s)
		{
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				}
				else
				{
					out += static_cast<char>(c);
				}
			}
		}
		out += '"';
	}

	// Reads t[key] (t at stack index 1) as a string, if it is one.
	bool FieldString(ILuaBase* L, int tableIdx, const char* key, size_t max, std::string& out)
	{
		L->GetField(tableIdx, key);
		bool ok = L->IsType(-1, Type::String);
		if (ok)
		{
			unsigned int len = 0;
			const char* s = L->GetString(-1, &len);
			out = Truncate(s, len, max);
		}
		L->Pop();
		return ok && !out.empty();
	}

	bool FieldNumber(ILuaBase* L, int tableIdx, const char* key, double& out)
	{
		L->GetField(tableIdx, key);
		bool ok = L->IsType(-1, Type::Number);
		if (ok)
			out = L->GetNumber(-1);
		L->Pop();
		return ok;
	}

	// https only: a button on someone's Discord profile shouldn't lead anywhere in the clear.
	bool IsHttpUrl(const std::string& url)
	{
		return url.rfind("https://", 0) == 0;
	}

	std::string BuildActivity(ILuaBase* L)
	{
		std::string json = "{";
		bool first = true;
		auto comma = [&] { if (!first) json += ','; first = false; };

		std::string s;
		// Discord needs at least 2 characters in these.
		if (FieldString(L, 1, "details", MAX_TEXT, s) && s.size() >= 2)
		{
			comma(); json += "\"details\":"; AppendJsonString(json, s);
		}
		if (FieldString(L, 1, "state", MAX_TEXT, s) && s.size() >= 2)
		{
			comma(); json += "\"state\":"; AppendJsonString(json, s);
		}

		double start = 0;
		if (FieldNumber(L, 1, "start", start) && start > 0)
		{
			comma(); json += "\"timestamps\":{\"start\":" + std::to_string(static_cast<long long>(start)) + "}";
		}

		std::string image, imageText;
		bool hasImage = FieldString(L, 1, "large_image", MAX_URL, image);
		bool hasImageText = FieldString(L, 1, "large_text", MAX_TEXT, imageText) && imageText.size() >= 2;
		if (hasImage || hasImageText)
		{
			comma(); json += "\"assets\":{";
			if (hasImage) { json += "\"large_image\":"; AppendJsonString(json, image); }
			if (hasImageText) { if (hasImage) json += ','; json += "\"large_text\":"; AppendJsonString(json, imageText); }
			json += '}';
		}

		double size = 0, max = 0;
		if (FieldNumber(L, 1, "party_size", size) && FieldNumber(L, 1, "party_max", max) && size >= 1 && max >= size)
		{
			comma();
			json += "\"party\":{\"id\":\"meowy\",\"size\":[" + std::to_string(static_cast<long long>(size)) + "," +
				std::to_string(static_cast<long long>(max)) + "]}";
		}

		L->GetField(1, "buttons");
		if (L->IsType(-1, Type::Table))
		{
			std::string buttons;
			int count = 0;
			for (int i = 1; i <= 2; ++i)
			{
				L->PushNumber(i);
				L->GetTable(-2);
				if (L->IsType(-1, Type::Table))
				{
					std::string label, url;
					int btn = L->Top();
					if (FieldString(L, btn, "label", MAX_LABEL, label) && FieldString(L, btn, "url", MAX_URL, url) && IsHttpUrl(url))
					{
						if (count++ > 0) buttons += ',';
						buttons += "{\"label\":";
						AppendJsonString(buttons, label);
						buttons += ",\"url\":";
						AppendJsonString(buttons, url);
						buttons += '}';
					}
				}
				L->Pop();
			}
			if (count > 0)
			{
				comma(); json += "\"buttons\":[" + buttons + "]";
			}
		}
		L->Pop();

		json += '}';
		return json;
	}

	bool IsSnowflake(const char* s)
	{
		size_t n = 0;
		for (; s[n]; ++n)
			if (s[n] < '0' || s[n] > '9')
				return false;
		return n >= 15 && n <= 25;
	}

	// Pushes `false, reason` and returns true when this game isn't connected to one
	// of our servers. Every call that reads the player's data or acts for them
	// goes through this; see server_gate.h for why it can't be done in Lua.
	bool Refuse(ILuaBase* LUA, int& results)
	{
		server_gate::Result r = server_gate::Check();
		if (r == server_gate::Result::Allowed)
			return false;
		LUA->PushBool(false);
		LUA->PushString(server_gate::Describe(r));
		results = 2;
		return true;
	}

	// meowy.ServerAllowed() -> true | false, reason
	int ServerAllowed(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		int results = 0;
		if (Refuse(LUA, results))
			return results;
		LUA->PushBool(true);
		return 1;
	}

	int DiscordStart(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		int results = 0;
		if (Refuse(LUA, results))
			return results;
		const char* id = LUA->CheckString(1);
		if (!IsSnowflake(id))
			LUA->ArgError(1, "not a Discord application id");
		discord::Start(id);
		return 0;
	}

	int DiscordSetActivity(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		int results = 0;
		if (Refuse(LUA, results))
			return results;
		LUA->CheckType(1, Type::Table);
		discord::SetActivity(BuildActivity(LUA));
		return 0;
	}

	int DiscordClear(lua_State*)
	{
		discord::SetActivity("null");
		return 0;
	}

	int DiscordState(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		LUA->PushString(discord::State());
		return 1;
	}

	// Menu only: meowy.DiscordAllow(on), the player's Discord status switch
	// (meowy_discord in Meowy settings, read the menu's way). Off: what Discord
	// shows is cleared at once, and nothing a server asks for reaches Discord.
	// The client realm has no such function: a server can't turn it back on.
	int DiscordAllow(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		discord::SetAllowed(LUA->GetBool(1));
		return 0;
	}

	int DiscordLastError(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		int results = 0;
		if (Refuse(LUA, results))
			return results;
		std::string err = discord::LastError();
		if (err.empty())
			LUA->PushNil();
		else
			LUA->PushString(err.c_str(), static_cast<unsigned int>(err.size()));
		return 1;
	}

#ifdef _WIN32
	std::wstring DataDir();
#endif

	// Only crashes on Meowy Network (resilience::OurCrashes): a report from a
	// crash on another server names that server's Lua files, and a report
	// can't say where the game was, so one the pack didn't note as ours is
	// never sent. The folder is listed first and filtered to ours by name,
	// newest first, and only then are reports read, one at a time, until
	// `want` have a Lua trace: this runs on the main thread, often during a
	// join, and a long-time player's crashes folder holds hundreds of reports
	// and dumps. (Filtering after reading also let 20 newer reports from other
	// servers hide every one of ours.)
	int CrashReports(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		server_gate::Result gate = server_gate::Check();
		if (gate != server_gate::Result::Allowed)
		{
			LUA->CreateTable();
			LUA->PushString(server_gate::Describe(gate));
			return 2;
		}
		long long since = static_cast<long long>(LUA->CheckNumber(1));
		double max = LUA->IsType(2, Type::Number) ? LUA->GetNumber(2) : 5;
		const size_t want = max < 1 ? 1 : max > 20 ? 20 : static_cast<size_t>(max);
		std::vector<crashes::Report> reports;
#ifdef _WIN32
		{
			const std::wstring root = crashes::Root();
			std::vector<crashes::Candidate> listed = crashes::Candidates(root, since);
			std::vector<std::string> names;
			names.reserve(listed.size());
			for (const crashes::Candidate& c : listed)
				names.push_back(c.name);
			const std::vector<std::string> ours = resilience::OurCrashes(DataDir(), names);
			listed.erase(std::remove_if(listed.begin(), listed.end(), [&ours](const crashes::Candidate& c) {
				return std::find(ours.begin(), ours.end(), c.name) == ours.end();
			}), listed.end());
			std::stable_sort(listed.begin(), listed.end(), [](const crashes::Candidate& a, const crashes::Candidate& b) {
				return a.time > b.time;
			});
			for (const crashes::Candidate& c : listed)
			{
				if (reports.size() >= want)
					break;
				crashes::Report r;
				if (crashes::Read(root, c, r))
					reports.push_back(std::move(r));
			}
		}
#endif

		LUA->CreateTable();
		int i = 0;
		for (const crashes::Report& r : reports)
		{
			LUA->PushNumber(++i);
			LUA->CreateTable();
			LUA->PushNumber(static_cast<double>(r.time));
			LUA->SetField(-2, "time");
			LUA->PushString(r.luaTrace.c_str(), static_cast<unsigned int>(r.luaTrace.size()));
			LUA->SetField(-2, "lua");
			LUA->PushString(r.fault.c_str(), static_cast<unsigned int>(r.fault.size()));
			LUA->SetField(-2, "fault");
			LUA->PushString(r.faultModule.c_str(), static_cast<unsigned int>(r.faultModule.size()));
			LUA->SetField(-2, "fault_module");
			LUA->PushNumber(static_cast<double>(r.faultOffset));
			LUA->SetField(-2, "fault_offset");
			LUA->PushNumber(static_cast<double>(r.faultCode));
			LUA->SetField(-2, "fault_code");
			LUA->SetTable(-3);
		}
		return 1;
	}

	// meowy.SkipMaterialReload() -> true | false, reason
	int SkipMaterialReload(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		int results = 0;
		if (Refuse(LUA, results))
			return results;
		std::string why;
		bool ok = engine_patches::SkipMaterialReload(why);
		LUA->PushBool(ok);
		if (ok)
			return 1;
		LUA->PushString(why.c_str());
		return 2;
	}

	int RestoreMaterialReload(lua_State* L)
	{
		L->luabase->PushBool(engine_patches::RestoreMaterialReload());
		return 1;
	}

	// meowy.SkipMaterialReloadAnyServer() -> true | false, reason. Menu realm
	// only: the player's own opt-in (meowy_skipmatreload_anyserver) for joins to
	// any server, applied while a join loads and put back once it's in game.
	int SkipMaterialReloadAnyServer(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		std::string why;
		const bool ok = engine_patches::SkipMaterialReload(why);
		LUA->PushBool(ok);
		if (ok)
			return 1;
		LUA->PushString(why.c_str());
		return 2;
	}

	int MaterialReloadState(lua_State* L)
	{
		L->luabase->PushString(engine_patches::MaterialReloadState());
		return 1;
	}

	// meowy.FastLuaLookup(on) -> true | false, reason. Menu realm only: the
	// patch must outlive any one server, and this DLL is pinned there.
	int FastLuaLookup(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		if (!LUA->GetBool(1))
		{
			lua_lookup::Uninstall();
			LUA->PushBool(true);
			return 1;
		}
		std::string why;
		const bool ok = lua_lookup::Install(why);
		LUA->PushBool(ok);
		if (ok)
			return 1;
		LUA->PushString(why.c_str());
		return 2;
	}

	int FastLuaLookupState(lua_State* L)
	{
		L->luabase->PushString(lua_lookup::Describe().c_str());
		return 1;
	}

	int FastLuaLookupNewJoin(lua_State*)
	{
		lua_lookup::NewJoin();
		lua_find::NewJoin();
		return 0;
	}

	// meowy.FastLuaFind(on) -> true | false, reason (lua_find.h). Menu realm
	// only, like FastLuaLookup.
	int FastLuaFind(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		if (!LUA->GetBool(1))
		{
			lua_find::Uninstall();
			LUA->PushBool(true);
			return 1;
		}
		std::string why;
		const bool ok = lua_find::Install(why);
		LUA->PushBool(ok);
		if (ok)
			return 1;
		LUA->PushString(why.c_str());
		return 2;
	}

	int FastLuaFindState(lua_State* L)
	{
		L->luabase->PushString(lua_find::Describe().c_str());
		return 1;
	}

	// meowy.FastFonts(on) -> true | false, reason (font_loads.h). Menu realm
	// only, like FastLuaLookup.
	int FastFonts(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		if (!LUA->GetBool(1))
		{
			font_loads::Uninstall();
			LUA->PushBool(true);
			return 1;
		}
		std::string why;
		const bool ok = font_loads::Install(why);
		LUA->PushBool(ok);
		if (ok)
			return 1;
		LUA->PushString(why.c_str());
		return 2;
	}

	// meowy.MouseConfine(on): hold the mouse inside the game window while aiming (mouse_confine.h).
	int MouseConfine(lua_State* L)
	{
		mouse_confine::Set(L->luabase->GetBool(1));
		return 0;
	}

	int MouseConfineState(lua_State* L)
	{
		L->luabase->PushString(mouse_confine::Describe().c_str());
		return 1;
	}

	int FastFontsState(lua_State* L)
	{
		L->luabase->PushString(font_loads::Describe().c_str());
		return 1;
	}

	// meowy.FastFileChecks(on) -> true | false, reason (fs_cache.h). Menu realm only.
	int FastFileChecks(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		if (!LUA->GetBool(1))
		{
			fs_cache::Uninstall();
			LUA->PushBool(true);
			return 1;
		}
		std::string why;
		const bool ok = fs_cache::Install(why);
		LUA->PushBool(ok);
		if (ok)
			return 1;
		LUA->PushString(why.c_str());
		return 2;
	}

	// meowy.FastFileChecksActive(on): the join window.
	int FastFileChecksActive(lua_State* L)
	{
		fs_cache::SetActive(L->luabase->GetBool(1));
		return 0;
	}

	int FastFileChecksState(lua_State* L)
	{
		L->luabase->PushString(fs_cache::Describe().c_str());
		return 1;
	}

	int Hardware(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		server_gate::Result gate = server_gate::Check();
		if (gate != server_gate::Result::Allowed)
		{
			LUA->PushNil();
			LUA->PushString(server_gate::Describe(gate));
			return 2;
		}
		hardware::Info hw = hardware::Get();
		LUA->CreateTable();
		LUA->PushString(hw.cpu.c_str());
		LUA->SetField(-2, "cpu");
		LUA->PushNumber(hw.threads);
		LUA->SetField(-2, "threads");
		LUA->PushNumber(static_cast<double>(hw.ramMb));
		LUA->SetField(-2, "ram_mb");
		LUA->PushString(hw.gpu.c_str());
		LUA->SetField(-2, "gpu");
		LUA->PushNumber(static_cast<double>(hw.vramMb));
		LUA->SetField(-2, "vram_mb");
		LUA->PushString(hw.os.c_str());
		LUA->SetField(-2, "os");
		return 1;
	}

	void SetFunction(ILuaBase* L, const char* name, CFunc fn)
	{
		L->PushCFunction(fn);
		L->SetField(-2, name);
	}

	// Every function Lua can call is registered through this. A C++ exception
	// (std::bad_alloc in a 32-bit game, say) must never unwind into the game's
	// Lua: it becomes `false, "internal error"`, returned after every C++ local
	// of the function is gone. Lua's own errors (CheckString's argument errors)
	// aren't std::exceptions and pass through untouched.
	template <CFunc F>
	int Guarded(lua_State* L)
	{
		const int top = L->luabase->Top();
		try
		{
			return F(L);
		}
		catch (const std::exception&)
		{
		}
		const int pushed = L->luabase->Top() - top;
		if (pushed > 0)
			L->luabase->Pop(pushed);
		L->luabase->PushBool(false);
		L->luabase->PushString("internal error");
		return 2;
	}

	// --- Loading screen (loading_screen.h) ----------------------------------

#ifdef _WIN32
	// garrysmod\data\meowy, from this DLL's own path (garrysmod\lua\bin): where
	// the pack kept its files before 1.4.1.
	std::wstring LegacyDataDir()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&LegacyDataDir), &self);
		wchar_t path[MAX_PATH];
		DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
		std::wstring s(path, n < MAX_PATH ? n : 0);
		size_t slash = s.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1) + L"..\\..\\data\\meowy";
	}

	// %LOCALAPPDATA%\MeowyClientPack. Not garrysmod\data, which every server's
	// Lua can read (file.Read): a file there would tell any server that this
	// player has the pack. The first call moves the join times over from the
	// old folder and removes the old files.
	std::wstring DataDir()
	{
		static std::wstring dir;
		if (!dir.empty())
			return dir;

		wchar_t base[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return LegacyDataDir();
		dir = std::wstring(base) + L"\\MeowyClientPack";
		CreateDirectoryW(dir.c_str(), nullptr);

		const std::wstring old = LegacyDataDir();
		if (!old.empty())
		{
			MoveFileW((old + L"\\join_times.txt").c_str(), (dir + L"\\join_times.txt").c_str());
			for (const wchar_t* name : { L"join_times.txt", L"last_join.txt", L"css_scale.txt", L"boot.lock" })
				DeleteFileW((old + L"\\" + name).c_str());
			RemoveDirectoryW(old.c_str()); // only if nothing else is left in it
		}
		return dir;
	}

	// The game's garrysmod folder: two up from its lua\bin, where the installer
	// put the start-up stub and the pack. An update runs from
	// %LOCALAPPDATA%\MeowyClientPack\bin\<version> (pack_versions.h), so this
	// DLL's own folder says nothing then: the stub's does (loaded for the whole
	// game in the menu state), or the installed copy's (a server's require).
	// 1.7.0 and 1.7.1 took this DLL's own path, so an update looked for config.cfg
	// and menu.lua in %LOCALAPPDATA%: no settings backups, and "Start with the
	// game" showing off.
	std::wstring ModulePath(HMODULE m)
	{
		wchar_t path[MAX_PATH * 2];
		const DWORD n = m ? GetModuleFileNameW(m, path, MAX_PATH * 2) : 0;
		return std::wstring(path, n < MAX_PATH * 2 ? n : 0);
	}

	std::wstring GarrysModDir()
	{
		static std::wstring dir;
		if (!dir.empty())
			return dir;
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&GarrysModDir), &self);
		const std::wstring stub = std::wstring(L"gmsv_meowy_boot_") + pack_versions::Arch() + L".dll";
		for (HMODULE m : { self, GetModuleHandleW(stub.c_str()), GetModuleHandleW(pack_versions::DllName().c_str()) })
		{
			const std::wstring found = config_backup::GameDirOf(ModulePath(m));
			if (!found.empty())
				return dir = found;
		}
		// Neither (not expected): as before, two up from this DLL, not kept.
		const std::wstring s = ModulePath(self);
		const size_t slash = s.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1) + L"..\\..";
	}

	// --- Start with the game (startup_hook.h) ------------------------------------

	// Menu: meowy.StartupHook() -> on (the line is in menu.lua), off (the player
	// turned it off).
	int StartupHookState(lua_State* L)
	{
		L->luabase->PushBool(startup_hook::Present(GarrysModDir()));
		L->luabase->PushBool(startup_hook::Off(DataDir()));
		return 2;
	}

	// Menu: meowy.SetStartupHook(on) -> ok. Meowy settings' "Start with the game".
	int SetStartupHook(lua_State* L)
	{
		L->luabase->PushBool(startup_hook::Set(GarrysModDir(), DataDir(), L->luabase->GetBool(1)));
		return 1;
	}
#endif

	// --- Notifications (notify.h, notify_toast.h) -------------------------------

	notify::RateLimiter g_notifyLimiter;
	int g_notifyEnabled = -1;   // -1: not read from notify.txt yet

#ifdef _WIN32
	std::wstring NotifySettingsFile()
	{
		return DataDir() + L"\\notify.txt";
	}
#endif

	bool NotifyEnabledSetting()
	{
#ifdef _WIN32
		if (g_notifyEnabled < 0)
			g_notifyEnabled = notify::LoadEnabled(NotifySettingsFile()) ? 1 : 0;
		return g_notifyEnabled == 1;
#else
		return false;
#endif
	}

	// --- The signed notification catalogue (notify_catalogue.h) ------------------

	bool g_catalogueLoaded = false;

#ifdef _WIN32
	// One file: the signature's 128 hex digits, a newline, then the body. Read
	// back through the same checks as one from the server.
	std::wstring CatalogueFile()
	{
		return DataDir() + L"\\notify_catalogue.txt";
	}
#endif

	// The last body and signature that passed Verify. Our server sends the same
	// catalogue at every join, usually the one already kept on disk: the same
	// bytes skip the signature check (ECDSA through freshly opened BCrypt
	// providers, a few ms on the main thread). Parse and the version check
	// still run every time.
	std::string g_verifiedBody, g_verifiedSig;

	// Verifies, parses and installs a catalogue. True when it's in use (a
	// re-send of the current version included); else false with *why.
	bool TakeCatalogue(const std::string& body, const std::string& sig, bool persist, std::string* why)
	{
		const bool known = !g_verifiedSig.empty() && sig == g_verifiedSig && body == g_verifiedBody;
		if (!known)
		{
			if (!notify_catalogue::Verify(body, sig))
			{
				*why = "bad signature";
				return false;
			}
			g_verifiedBody = body;
			g_verifiedSig = sig;
		}
		notify_catalogue::Catalogue c;
		const char* parseWhy = nullptr;
		if (!notify_catalogue::Parse(body, &c, &parseWhy))
		{
			*why = std::string("bad catalogue: ") + (parseWhy ? parseWhy : "unknown");
			return false;
		}
		uint64_t current = notify::CatalogueVersion();
		if (c.version < current)
		{
			*why = "older catalogue";
			return false;
		}
		if (c.version == current)
			return true;
		if (!notify::Install(c))
		{
			*why = "older catalogue";
			return false;
		}
		g_notifyLimiter.ResetKinds();
#ifdef _WIN32
		if (persist)
			file_util::WriteAtomic(CatalogueFile(), sig + "\n" + body);
#else
		(void)persist;
#endif
		return true;
	}

	// The catalogue kept from an earlier game, once per game. A missing, edited
	// or older file just leaves the compiled list (or what the server sends).
	void EnsureCatalogueLoaded()
	{
		if (g_catalogueLoaded)
			return;
		g_catalogueLoaded = true;
#ifdef _WIN32
		std::string saved = file_util::ReadSmall(CatalogueFile(), notify_catalogue::kSigHexLen + 1 + notify_catalogue::kMaxBody);
		if (saved.size() <= notify_catalogue::kSigHexLen + 1 || saved[notify_catalogue::kSigHexLen] != '\n')
			return;
		std::string why;
		TakeCatalogue(saved.substr(notify_catalogue::kSigHexLen + 1), saved.substr(0, notify_catalogue::kSigHexLen),
			false, &why);
#endif
	}

	// meowy.NotifyCatalogue(body, sig) -> true, version | false, reason
	int NotifyCatalogue(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		int results = 0;
		if (Refuse(LUA, results))
			return results;
		std::string why;
		bool ok = false;
		try
		{
			unsigned int bodyLen = 0, sigLen = 0;
			const char* body = LUA->IsType(1, Type::String) ? LUA->GetString(1, &bodyLen) : nullptr;
			const char* sig = LUA->IsType(2, Type::String) ? LUA->GetString(2, &sigLen) : nullptr;
			if (!body || !sig || bodyLen == 0 || bodyLen > notify_catalogue::kMaxBody ||
				sigLen != notify_catalogue::kSigHexLen)
				why = "bad catalogue: bad arguments";
			else
			{
				EnsureCatalogueLoaded();
				ok = TakeCatalogue(std::string(body, bodyLen), std::string(sig, sigLen), true, &why);
			}
		}
		catch (...)
		{
			ok = false;
			why = "internal error";
		}
		if (!ok)
		{
			LUA->PushBool(false);
			LUA->PushString(why.c_str());
			return 2;
		}
		LUA->PushBool(true);
		LUA->PushNumber(static_cast<double>(notify::CatalogueVersion()));
		return 2;
	}

	// meowy.Notify(kind) -> true | false, reason. Only the kind is read; the
	// words shown are the pack's. Never blocks: the worker thread shows it.
	int Notify(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* kind = nullptr;
		unsigned int len = 0;
		if (LUA->IsType(1, Type::String))
			kind = LUA->GetString(1, &len);

		const char* reason = nullptr;
		try
		{
			notify::Inputs in;
			in.ourServer = server_gate::Check() == server_gate::Result::Allowed;
			// The rest only matters (and is only worked out) for our server.
			if (in.ourServer)
			{
				EnsureCatalogueLoaded();
				in.enabled = NotifyEnabledSetting();
				in.focused = notify_toast::GameFocused();
#ifdef _WIN32
				in.nowMs = GetTickCount64();
#endif
			}
			int index = -1;
			notify::Refusal r = notify::Decide(kind, len, in, g_notifyLimiter, &index);
			if (r != notify::Refusal::None)
				reason = notify::Describe(r);
			else if (!notify_toast::Post(index))
				reason = "couldn't start the notification thread";
		}
		catch (...)
		{
			reason = "internal error";
		}

		LUA->PushBool(reason == nullptr);
		if (!reason)
			return 1;
		LUA->PushString(reason);
		return 2;
	}

	// meowy.SetNotifyEnabled(on) -> true | false, reason
	int SetNotifyEnabled(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		int results = 0;
		if (Refuse(LUA, results))
			return results;
		bool on = LUA->GetBool(1);   // anything but false/nil is "on"
		bool saved = false;
		try
		{
#ifdef _WIN32
			g_notifyEnabled = on ? 1 : 0;
			saved = notify::SaveEnabled(NotifySettingsFile(), on);
#endif
		}
		catch (...)
		{
			saved = false;
		}
		LUA->PushBool(saved);
		if (saved)
			return 1;
		LUA->PushString("couldn't save the setting");
		return 2;
	}

	// meowy.NotifyEnabled() -> true | false, or nil, reason
	int NotifyEnabled(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		server_gate::Result gate = server_gate::Check();
		if (gate != server_gate::Result::Allowed)
		{
			LUA->PushNil();
			LUA->PushString(server_gate::Describe(gate));
			return 2;
		}
		bool on = false;
		try
		{
			on = NotifyEnabledSetting();
		}
		catch (...)
		{
		}
		LUA->PushBool(on);
		return 1;
	}

	// --- Steam Timeline markers (timeline.h, timeline_steam.h) -------------------

	timeline::RateLimiter g_timelineLimiter;
	bool g_timelineLoading = false;   // the bar is in LoadingScreen mode for one of our joins

	// meowy.TimelineEvent(kind) -> true | false, reason. Only the kind is read;
	// what Steam shows is the pack's. One short call into the local Steam client.
	int TimelineEvent(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* kind = nullptr;
		unsigned int len = 0;
		if (LUA->IsType(1, Type::String))
			kind = LUA->GetString(1, &len);

		const char* reason = nullptr;
		try
		{
			timeline::Inputs in;
			in.ourServer = server_gate::Check() == server_gate::Result::Allowed;
			// Steam is only asked once the gate and the kind have passed.
			if (in.ourServer && timeline::FindKind(kind, len) >= 0)
			{
				in.available = timeline_steam::Available();
#ifdef _WIN32
				in.nowMs = GetTickCount64();
#endif
			}
			int index = -1;
			timeline::Refusal r = timeline::Decide(kind, len, in, g_timelineLimiter, &index);
			if (r != timeline::Refusal::None)
				reason = timeline::Describe(r);
			else if (!timeline_steam::Add(index))
				reason = timeline::Describe(timeline::Refusal::Unavailable);
		}
		catch (...)
		{
			reason = "internal error";
		}

		LUA->PushBool(reason == nullptr);
		if (!reason)
			return 1;
		LUA->PushString(reason);
		return 2;
	}

	// Menu: the join to our server shows as loading on Steam's timeline, so a
	// recording's "Playing" part starts when the player is in.
	void TimelineLoading(bool loading)
	{
		if (loading == g_timelineLoading)
			return;
		g_timelineLoading = loading;
		timeline_steam::SetMode(loading ? timeline_steam::Mode::LoadingScreen : timeline_steam::Mode::Playing);
	}

	// Menu: is the join that's loading one to our server? Same check as everything else.
	int LoadingTarget(lua_State* L)
	{
		L->luabase->PushBool(server_gate::Check() == server_gate::Result::Allowed);
		return 1;
	}

	// LoadingBegin(ms): ms = how long GMod's own loading panel had been open.
	int LoadingBegin(lua_State* L)
	{
		// Either the engine's real connection is ours, or it has just printed
		// that it's connecting to one of ours (connect_watch.h).
		if (server_gate::Check() != server_gate::Result::Allowed && !connect_watch::HeadingToUs(10000))
			return 0;
		connect_watch::JoinStarted();
#ifdef _WIN32
		ILuaBase* LUA = L->luabase;
		loading_screen::Begin(DataDir(), LUA->IsType(1, Type::Number) ? LUA->GetNumber(1) : -1);
		loading_screen::Note("watch", connect_watch::Describe());
#endif
		TimelineLoading(true);
		return 0;
	}

	// LoadingBeginOther(ms, name, map): another server's join (meowy_loadingscreen_everywhere),
	// only once the engine's connection is to it and not one of ours. Its address
	// (for its own stage times) is the engine's, read here.
	int LoadingBeginOther(lua_State* L)
	{
		std::string address;
		if (server_gate::Check(&address) != server_gate::Result::OtherServer)
			return 0;
#ifdef _WIN32
		ILuaBase* LUA = L->luabase;
		loading_screen::BeginOther(DataDir(), LUA->IsType(1, Type::Number) ? LUA->GetNumber(1) : -1,
			LUA->IsType(2, Type::String) ? LUA->GetString(2) : "", LUA->IsType(3, Type::String) ? LUA->GetString(3) : "", address);
#endif
		TimelineLoading(true);
		return 0;
	}

	// The server's name, the map and the player's SteamID go to the page only
	// once the real connection is to one of our servers.
	int LoadingDetails(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		if (server_gate::Check() != server_gate::Result::Allowed)
			return 0;
		loading_screen::Details(LUA->CheckString(1), LUA->CheckString(2),
			LUA->IsType(3, Type::String) ? LUA->GetString(3) : "");
		return 0;
	}

	// Menu: before the connection exists, the join is "heading to us" from the
	// engine's own "Connecting to ..." line. Then: "ours", "other" or "waiting".
	int LoadingHeadsUp(lua_State* L)
	{
		L->luabase->PushBool(connect_watch::HeadingToUs(10000));
		return 1;
	}

	int LoadingTargetState(lua_State* L)
	{
		switch (server_gate::Check())
		{
		case server_gate::Result::Allowed: L->luabase->PushString("ours"); break;
		case server_gate::Result::OtherServer: L->luabase->PushString("other"); break;
		default: L->luabase->PushString("waiting"); break;
		}
		return 1;
	}

	int LoadingStatus(lua_State* L)
	{
		loading_screen::Status(L->luabase->CheckString(1));
		return 0;
	}

	int LoadingCancelRequested(lua_State* L)
	{
		L->luabase->PushBool(loading_screen::CancelRequested());
		return 1;
	}

	int LoadingInGame(lua_State*)
	{
		TimelineLoading(false);
		loading_screen::InGame();
		return 0;
	}

	// Menu: load the screen's page (WebView2) now, hidden, so a join shows it at once.
	int LoadingPrewarm(lua_State*)
	{
#ifdef _WIN32
		loading_screen::Prewarm(DataDir());
#endif
		return 0;
	}

	// Menu: unload it once no join is on screen (in game, or a menu left in the
	// background); the next LoadingPrewarm or join starts it again.
	int LoadingPark(lua_State*)
	{
#ifdef _WIN32
		loading_screen::Park();
#endif
		return 0;
	}

	int LoadingEnd(lua_State*)
	{
		TimelineLoading(false);
		loading_screen::End();
		return 0;
	}

	// Client: the player has spawned and the game runs smoothly; fade out.
	int LoadingDone(lua_State*)
	{
		// Any server's client Lua can call this; only ours ends our screen.
		if (server_gate::Check() == server_gate::Result::Allowed)
			loading_screen::Done();
		return 0;
	}

	// --- Honest server list (server_rep.h) -------------------------------------

	int ServerRepFetch(lua_State*)
	{
		server_rep::FetchList();
		return 0;
	}

	// The verdicts JSON when a fetch has finished since the last call, else nil.
	// The server list's Think asks every frame, on the game's thread: the reports'
	// Steam tickets the backend is done with are cancelled here (steam_ticket.h).
	int ServerRepTake(lua_State* L)
	{
		steam_ticket::Pump();
		std::string json = server_rep::TakeList();
		if (json.empty())
			L->luabase->PushNil();
		else
			L->luabase->PushString(json.c_str(), (unsigned int)json.size());
		return 1;
	}

	int ServerRepReport(lua_State* L)
	{
		if (L->luabase->IsType(1, Type::String))
			server_rep::SendReport(L->luabase->GetString(1));
		return 0;
	}

	// meowy.QueryPlayers(address) -> true if asked, false if not (too many in
	// flight, or not a public address): then no answer is coming.
	int QueryPlayers(lua_State* L)
	{
		bool asked = L->luabase->IsType(1, Type::String) && server_rep::QueryPlayers(L->luabase->GetString(1));
		L->luabase->PushBool(asked);
		return 1;
	}

	// JSON array of finished player-list queries, or nil.
	int TakePlayers(lua_State* L)
	{
		std::string json = server_rep::TakePlayers();
		if (json.empty())
			L->luabase->PushNil();
		else
			L->luabase->PushString(json.c_str(), (unsigned int)json.size());
		return 1;
	}

	int IsOurServer(lua_State* L)
	{
		L->luabase->PushBool(L->luabase->IsType(1, Type::String) && server_gate::IsOurAddress(L->luabase->GetString(1)));
		return 1;
	}

	// OurServerAddress() -> "a.b.c.d:port" of our main server.
	int OurServerAddress(lua_State* L)
	{
		L->luabase->PushString(server_gate::PrimaryAddress().c_str());
		return 1;
	}

	bool RejoinableAddress(const std::string& address);

	// ServerNow() -> address or nil, round trip ms, loss 0..1, seconds connected
	// (each nil when unreadable). The address is the engine's own connection,
	// given only when it's ours or a public a.b.c.d:port (never LAN, loopback or P2P).
	int ServerNow(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		std::string address;
		const server_gate::Result r = server_gate::Check(&address);
		const bool connected = r == server_gate::Result::Allowed || r == server_gate::Result::OtherServer;
		if (connected && RejoinableAddress(address))
			LUA->PushString(address.c_str());
		else
			LUA->PushNil();
		double latency = 0, loss = 0, since = 0;
		if (connected && server_gate::NetStats(latency, loss, since))
		{
			LUA->PushNumber(latency * 1000.0);
			LUA->PushNumber(loss);
			LUA->PushNumber(since);
		}
		else
		{
			LUA->PushNil();
			LUA->PushNil();
			LUA->PushNil();
		}
		return 4;
	}

	// DataSandbox(onServer): data/ belongs to the server being joined or played
	// (onServer true), or to the menu (false). See data_sandbox.h. Called every
	// frame; a switch only happens when the owner changes.
	int DataSandbox(lua_State* L)
	{
		bool onServer = L->luabase->IsType(1, Type::Bool) && L->luabase->GetBool(1);
		if (!onServer)
		{
			data_sandbox::Leave();
			return 0;
		}
		// Only the engine's own connection says whose folder it is. Never console
		// text: a server's Lua can print "Connecting to <another server>" itself.
		std::string address;
		switch (server_gate::Check(&address))
		{
		case server_gate::Result::Allowed:
		case server_gate::Result::OtherServer:
			break;
		case server_gate::Result::NotConnected:
			// Loading with no connection yet: a remote server's Lua can't run
			// before its connection exists (and then this switches to its folder),
			// while a game hosted here (singleplayer, listen server) runs its
			// server Lua before connecting to itself. Either way, the menu's.
			data_sandbox::Leave();
			return 0;
		case server_gate::Result::Unavailable:
			// The engine can't be asked (a game update): a fixed folder, never the menu's.
			data_sandbox::EnterUnknown();
			return 0;
		}
		// Singleplayer, or hosting a game here: the player's own game, the menu's files.
		if (address.find("loopback") != std::string::npos || address.rfind("localhost", 0) == 0)
			data_sandbox::Leave();
		else if (address.empty())
			data_sandbox::EnterUnknown();
		else
			data_sandbox::Enter(address);   // a P2P host or odd address gets a fixed folder of its own
		return 0;
	}

	int DataSandboxDescribe(lua_State* L)
	{
		L->luabase->PushString(data_sandbox::Describe().c_str());
		return 1;
	}

	// "ip:port" of the server the game is really connected to, when it's
	// someone else's (not ours, not singleplayer); else nil.
	// Only a public a.b.c.d:port: a Steam P2P game or a LAN server is someone's
	// own game, never reported.
	// LauncherState(state): the menu Lua's "menu", "joining" or "ingame" for the
	// Meowy Launcher's pipe, with the engine's own address (never one from Lua).
	int LauncherState(lua_State* L)
	{
		if (!L->luabase->IsType(1, Type::String))
			return 0;
		const std::string state = L->luabase->GetString(1);
		std::string address;
		if (state != "menu")
			server_gate::Check(&address);
		launcher_pipe::SetState(state, address);
		return 0;
	}

	// LauncherTakeJoin() -> an address the launcher asked to join (checked: ours
	// or public), or nil.
	int LauncherTakeJoin(lua_State* L)
	{
		const std::string join = launcher_pipe::TakeJoin();
		if (join.empty())
			L->luabase->PushNil();
		else
			L->luabase->PushString(join.c_str());
		return 1;
	}

	int OtherServerAddress(lua_State* L)
	{
		std::string address;
		if (server_gate::Check(&address) == server_gate::Result::OtherServer && server_rep::IsPublicAddress(address)
			&& !server_gate::IsOurAddress(address))
			L->luabase->PushString(address.c_str());
		else
			L->luabase->PushNil();
		return 1;
	}

	int StatusCaptureBegin(lua_State*)
	{
		server_rep::CaptureBegin();
		return 0;
	}

	int ServerRepNote(lua_State* L)
	{
		if (L->luabase->IsType(1, Type::String))
			server_rep::Note(L->luabase->GetString(1));
		return 0;
	}

	int StatusCaptureEnd(lua_State* L)
	{
		std::string text = server_rep::CaptureEnd();
		server_rep::Note("status captured, " + std::to_string(text.size()) + " bytes");
		L->luabase->PushString(text.c_str(), (unsigned int)text.size());
		return 1;
	}

	// --- PC doctor (doctor.h, src/lua/doctor*.lua) -------------------------------

	// Menu: meowy.DoctorFacts() -> { arch, gpu_time_known, on_battery, battery_saver,
	// power_mode, adapters = { { name, vram_mb, perf_rank, power_rank, share } } }.
	// Read-only, local; no adapter LUIDs or serials.
	int DoctorFacts(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		doctor::Facts f = doctor::Gather();
		LUA->CreateTable();
		LUA->PushString(f.arch.c_str());
		LUA->SetField(-2, "arch");
		LUA->PushBool(f.gpuTimeKnown);
		LUA->SetField(-2, "gpu_time_known");
		if (f.onBattery >= 0) { LUA->PushBool(f.onBattery == 1); LUA->SetField(-2, "on_battery"); }
		if (f.batterySaver >= 0) { LUA->PushBool(f.batterySaver == 1); LUA->SetField(-2, "battery_saver"); }
		if (!f.powerMode.empty()) { LUA->PushString(f.powerMode.c_str()); LUA->SetField(-2, "power_mode"); }
		LUA->CreateTable();
		int i = 0;
		for (const doctor::Adapter& a : f.adapters)
		{
			LUA->PushNumber(++i);
			LUA->CreateTable();
			LUA->PushString(a.name.c_str());
			LUA->SetField(-2, "name");
			LUA->PushNumber(static_cast<double>(a.vramMb));
			LUA->SetField(-2, "vram_mb");
			LUA->PushNumber(a.perfRank);
			LUA->SetField(-2, "perf_rank");
			LUA->PushNumber(a.powerRank);
			LUA->SetField(-2, "power_rank");
			if (a.share >= 0) { LUA->PushNumber(a.share); LUA->SetField(-2, "share"); }
			LUA->SetTable(-3);
		}
		LUA->SetField(-2, "adapters");
		// audio = { output, input, comms_input, output_handsfree, input_handsfree, inputs } (audio_watch.h)
		const audio_watch::Devices d = audio_watch::Read();
		if (d.ok)
		{
			LUA->CreateTable();
			LUA->PushString(d.output.c_str());
			LUA->SetField(-2, "output");
			LUA->PushString(d.input.c_str());
			LUA->SetField(-2, "input");
			LUA->PushString(d.commsInput.c_str());
			LUA->SetField(-2, "comms_input");
			LUA->PushBool(d.outputHandsFree);
			LUA->SetField(-2, "output_handsfree");
			LUA->PushBool(d.inputHandsFree);
			LUA->SetField(-2, "input_handsfree");
			LUA->PushNumber(d.inputs);
			LUA->SetField(-2, "inputs");
			LUA->SetField(-2, "audio");
		}
		return 1;
	}

	// Menu: meowy.AudioChanges() -> how many times Windows' default output changed
	// (watching starts on the first call), or nil if it can't be watched.
	int AudioChanges(lua_State* L)
	{
		if (audio_watch::Start())
			L->luabase->PushNumber(audio_watch::Changes());
		else
			L->luabase->PushNil();
		return 1;
	}

	int AudioState(lua_State* L)
	{
		L->luabase->PushString(audio_watch::Describe().c_str());
		return 1;
	}

	// Menu: meowy.DoctorOpen(id) -> bool. Only the doctor's fixed pages (doctor::Open).
	int DoctorOpen(lua_State* L)
	{
		bool ok = L->luabase->IsType(1, Type::String) && doctor::Open(L->luabase->GetString(1));
		L->luabase->PushBool(ok);
		return 1;
	}

	// --- Connection resilience (resilience.h, src/lua/resilience*.lua) -----------

	// meowy.ResilienceWatch() -> state, address, timingOut, silence. state: "ours",
	// "other", "none" (not connected) or "unknown"; the rest only when "ours":
	// the engine's own address of our server, and its net channel's IsTimingOut
	// and seconds since it last heard from the server (nil, nil when they
	// couldn't be read). The menu samples this every frame.
	int ResilienceWatch(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		std::string address;
		server_gate::Result r = server_gate::Check(&address);
		switch (r)
		{
		case server_gate::Result::Allowed: LUA->PushString("ours"); break;
		case server_gate::Result::OtherServer: LUA->PushString("other"); break;
		case server_gate::Result::NotConnected: LUA->PushString("none"); break;
		default: LUA->PushString("unknown"); break;
		}
		if (r != server_gate::Result::Allowed)
			return 1;
		LUA->PushString(address.c_str());
		bool timingOut = false;
		double silence = 0;
		if (server_gate::NetHealth(timingOut, silence))
		{
			LUA->PushBool(timingOut);
			LUA->PushNumber(silence);
		}
		else
		{
			LUA->PushNil();
			LUA->PushNil();
		}
		return 4;
	}

	// meowy.ResilienceLastDisconnect() -> reason, secondsAgo: the engine's last
	// "Disconnected: <reason>" line (connect_watch), or nil before the first.
	int ResilienceLastDisconnect(lua_State* L)
	{
		std::string reason;
		unsigned long long msAgo = 0;
		if (!connect_watch::LastDisconnect(reason, msAgo))
			return 0;
		L->luabase->PushString(reason.c_str());
		L->luabase->PushNumber(static_cast<double>(msAgo) / 1000.0);
		return 2;
	}

	// Menu: our server said it restarts soon (meowy.ExpectRestart), still in force.
	int ResilienceRestartExpected(lua_State* L)
	{
		L->luabase->PushBool(resilience::RestartExpected());
		return 1;
	}

	int ResilienceClearRestart(lua_State*)
	{
		resilience::ClearRestart();
		return 0;
	}

	// Menu: meowy.ResilienceProbe(address) -> bool: asks our server (A2S_INFO,
	// plain UDP, not through Steam) whether it's up. Our addresses only.
	int ResilienceProbe(lua_State* L)
	{
		bool started = L->luabase->IsType(1, Type::String) && resilience::StartProbe(L->luabase->GetString(1));
		L->luabase->PushBool(started);
		return 1;
	}

	// Menu: meowy.ResilienceProbeResult() -> nil (waiting) | answered, locked.
	int ResilienceProbeResult(lua_State* L)
	{
		bool locked = false;
		int r = resilience::ProbeResult(locked);
		if (r == 0)
		{
			L->luabase->PushNil();
			return 1;
		}
		L->luabase->PushBool(r > 0);
		L->luabase->PushBool(locked);
		return 2;
	}

	// Menu: meowy.ResilienceSteamOnline() -> true | false | nil (unknown).
	int ResilienceSteamOnline(lua_State* L)
	{
		int r = resilience::SteamOnline();
		if (r < 0)
			L->luabase->PushNil();
		else
			L->luabase->PushBool(r == 1);
		return 1;
	}

	// Client: meowy.ExpectRestart(seconds) -> true, seconds | false, reason.
	// Our server, about to restart, says so: if the connection ends within
	// `seconds` (30..600), the menu waits for the server to come back and
	// rejoins (for players who opted in). Only while connected to our server.
	int ExpectRestart(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		int results = 0;
		if (Refuse(LUA, results))
			return results;
		double seconds = LUA->CheckNumber(1);
		int used = resilience::ExpectRestart(seconds);
		LUA->PushBool(true);
		LUA->PushNumber(used);
		return 2;
	}

#ifdef _WIN32
	int ResilienceSettingsRead(lua_State* L)
	{
		std::string text = resilience::ReadSettings(DataDir());
		L->luabase->PushString(text.c_str(), static_cast<unsigned int>(text.size()));
		return 1;
	}

	int ResilienceSettingsWrite(lua_State* L)
	{
		bool ok = L->luabase->IsType(1, Type::String) && resilience::WriteSettings(DataDir(), L->luabase->GetString(1));
		L->luabase->PushBool(ok);
		return 1;
	}

	// The menu's own settings (meowy.Setting in the menu Lua). The game saves
	// client-realm Lua convars to cfg/client.vdf but not menu-state ones, so
	// without this every pack setting was back to its default at each start.
	constexpr size_t kMaxMenuSettings = 4096;

	int MenuSettingsRead(lua_State* L)
	{
		const std::string text = file_util::ReadSmall(DataDir() + L"\\menu_settings.txt", kMaxMenuSettings);
		L->luabase->PushString(text.c_str(), static_cast<unsigned int>(text.size()));
		return 1;
	}

	int MenuSettingsWrite(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		unsigned int len = 0;
		const char* text = LUA->IsType(1, Type::String) ? LUA->GetString(1, &len) : nullptr;
		const bool ok = text && len <= kMaxMenuSettings &&
			file_util::WriteAtomic(DataDir() + L"\\menu_settings.txt", std::string(text, len));
		LUA->PushBool(ok);
		return 1;
	}

	// The menu Lua's own small files in %LOCALAPPDATA%\MeowyClientPack, by kind
	// (only these): the settings guard's session (settings_guard.lua: a game
	// that closed on a server is asked about at the next start) and its list of
	// servers always put back (kept here, not in a convar any server's Lua can
	// read), and one-time repairs done (home.lua).
	constexpr size_t kMaxMenuFile = 16384;
	const wchar_t* MenuFileName(const char* kind)
	{
		static const struct { const char* kind; const wchar_t* file; } kFiles[] = {
			{ "settings_guard_session", L"settings_guard_session.txt" },
			{ "settings_guard_always", L"settings_guard_always.txt" },
			{ "repairs", L"repairs.txt" },
		};
		for (const auto& f : kFiles)
			if (kind && std::strcmp(kind, f.kind) == 0)
				return f.file;
		return nullptr;
	}

	// MenuFileRead(kind) -> its text ("" when there's none).
	int MenuFileRead(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const wchar_t* file = MenuFileName(LUA->IsType(1, Type::String) ? LUA->GetString(1) : nullptr);
		const std::string text = file ? file_util::ReadSmall(DataDir() + L"\\" + file, kMaxMenuFile) : std::string();
		LUA->PushString(text.c_str(), static_cast<unsigned int>(text.size()));
		return 1;
	}

	// MenuFileWrite(kind, text) -> ok. "" removes the file.
	int MenuFileWrite(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const wchar_t* file = MenuFileName(LUA->IsType(1, Type::String) ? LUA->GetString(1) : nullptr);
		unsigned int len = 0;
		const char* text = LUA->IsType(2, Type::String) ? LUA->GetString(2, &len) : nullptr;
		bool ok = false;
		if (file && text && len <= kMaxMenuFile)
		{
			const std::wstring path = DataDir() + L"\\" + file;
			ok = len == 0 ? (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
				: file_util::WriteAtomic(path, std::string(text, len));
		}
		LUA->PushBool(ok);
		return 1;
	}

	// A server a crash card may offer: ours, or any public a.b.c.d:port (never a
	// LAN, loopback or P2P address).
	bool RejoinableAddress(const std::string& address)
	{
		return server_gate::IsOurAddress(address) || server_rep::IsPublicAddress(address);
	}

	// ResilienceSession(true, name): on a server now, ours or any public one
	// (the address is the engine's, read here; the name is the server's, only
	// ever shown as text); false: left it.
	int ResilienceSession(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		if (LUA->IsType(1, Type::Bool) && LUA->GetBool(1))
		{
			std::string address;
			const server_gate::Result r = server_gate::Check(&address);
			if ((r == server_gate::Result::Allowed || r == server_gate::Result::OtherServer) && RejoinableAddress(address))
				resilience::MarkSession(DataDir(), address, LUA->IsType(2, Type::String) ? LUA->GetString(2) : "");
		}
		else
		{
			resilience::ClearSession(DataDir());
		}
		return 0;
	}

	// The server to offer a rejoin to after a crash: address, name; or nil.
	int ResilienceCrashedSession(lua_State* L)
	{
		std::string name;
		const std::string address = resilience::CrashedSession(DataDir(), &name);
		if (address.empty() || !RejoinableAddress(address))
		{
			L->luabase->PushNil();
			return 1;
		}
		L->luabase->PushString(address.c_str());
		L->luabase->PushString(name.c_str());
		return 2;
	}

	// --- Join pre-warm (prewarm.h) ---------------------------------------------------
	constexpr uint64_t kPrewarmBudget = 4ull << 30;   // bytes read per warm, at most

	// A join is loading: start recording its big files and the blocks it reads
	// (and stop any warming). The read hook goes in on the first join; if it
	// can't, meowy_prewarm_status says "read hook off".
	int PrewarmBeginJoin(lua_State*)
	{
		static bool hookTried = false;
		if (!hookTried)
		{
			hookTried = true;
			std::string why;
			prewarm::InstallReadHook(why);
		}
		prewarm::BeginJoin();
		return 0;
	}

	// --- Convars the pack changes for a while -----------------------------------------
	// fps_max_nofocus while a join loads, fps_max_menu at a menu left in the
	// background (home.lua): the player's own value is kept in cvar_restore.txt
	// ("name value" lines) while it's changed, so a crash or a quit in between
	// (the game saves config.cfg at quit) is put right at the next start. Only
	// these names, and only numbers, since the values are run as commands.
	const char* const kHeldCvars[] = { "fps_max_nofocus", "fps_max_menu" };
	std::wstring HeldCvarFile() { return DataDir() + L"\\cvar_restore.txt"; }

	bool HeldCvarName(const std::string& name)
	{
		for (const char* n : kHeldCvars)
			if (name == n)
				return true;
		return false;
	}

	bool NumberText(const std::string& v)
	{
		return !v.empty() && v.size() <= 12 && v.find_first_not_of("0123456789.") == std::string::npos;
	}

	using HeldCvars = std::vector<std::pair<std::string, std::string>>;

	HeldCvars ReadHeldCvars()
	{
		HeldCvars held;
		const std::string text = file_util::ReadSmall(HeldCvarFile(), 1024);
		size_t at = 0;
		while (at < text.size())
		{
			size_t end = text.find('\n', at);
			if (end == std::string::npos)
				end = text.size();
			std::string line = text.substr(at, end - at);
			at = end + 1;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			const size_t space = line.find(' ');
			if (space == std::string::npos)
				continue;
			std::string name = line.substr(0, space), value = line.substr(space + 1);
			bool dup = false;
			for (const auto& h : held)
				dup = dup || h.first == name;
			if (!dup && HeldCvarName(name) && NumberText(value))
				held.emplace_back(std::move(name), std::move(value));
		}
		return held;
	}

	void WriteHeldCvars(const HeldCvars& held)
	{
		if (held.empty())
		{
			DeleteFileW(HeldCvarFile().c_str());
			return;
		}
		std::string text;
		for (const auto& h : held)
			text += h.first + " " + h.second + "\n";
		file_util::WriteAtomic(HeldCvarFile(), text);
	}

	// meowy.CvarHold(name, value) -> whether it's kept: the player's own value,
	// before the pack changes it. A name already kept keeps its first value.
	int CvarHold(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const std::string name = LUA->CheckString(1), value = LUA->CheckString(2);
		if (!HeldCvarName(name) || !NumberText(value))
		{
			LUA->PushBool(false);
			return 1;
		}
		HeldCvars held = ReadHeldCvars();
		bool there = false;
		for (const auto& h : held)
			there = there || h.first == name;
		if (!there)
		{
			held.emplace_back(name, value);
			WriteHeldCvars(held);
		}
		LUA->PushBool(true);
		return 1;
	}

	// meowy.CvarRelease(name): the caller put it back; no longer kept.
	int CvarRelease(lua_State* L)
	{
		const std::string name = L->luabase->CheckString(1);
		HeldCvars held = ReadHeldCvars();
		const size_t before = held.size();
		held.erase(std::remove_if(held.begin(), held.end(),
			[&](const std::pair<std::string, std::string>& h) { return h.first == name; }), held.end());
		if (held.size() != before)
			WriteHeldCvars(held);
		return 0;
	}

	// meowy.CvarHeld() -> { name = value }: values a quit or a crash left to put back.
	int CvarHeld(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		LUA->CreateTable();
		for (const auto& h : ReadHeldCvars())
		{
			LUA->PushString(h.second.c_str());
			LUA->SetField(-2, h.first.c_str());
		}
		return 1;
	}

	// --- Join priority guard ---------------------------------------------------------
	// While a join loads, the game asks Windows for full speed: its own QoS tag
	// (otherwise Windows lowers an unfocused or minimised game to Medium or Low:
	// efficiency clocks or cores on battery, and its 1 ms timer ignored once
	// occluded). Its fps_max_nofocus (20 by default) is lifted in Lua and kept
	// with CvarHold. The tag belongs to the process: it ends with it.
	bool SetJoinQoS(bool on)
	{
		using SetInfoFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
		static const SetInfoFn set = reinterpret_cast<SetInfoFn>(
			GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"));
		if (!set)
			return false;
		PROCESS_POWER_THROTTLING_STATE s = {};
		s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		// EXECUTION_SPEED (1) | IGNORE_TIMER_RESOLUTION (4), state 0: never throttle,
		// always honour the timer resolution. Off: both back to Windows' own policy.
		s.ControlMask = on ? (PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4) : 0;
		s.StateMask = 0;
		if (set(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)))
			return true;
		if (!on)
			return false;
		s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;   // Windows 10: no timer flag
		return set(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s)) != FALSE;
	}

	// meowy.JoinBoost(on) -> whether Windows took the QoS tag.
	int JoinBoost(lua_State* L)
	{
		L->luabase->PushBool(SetJoinQoS(L->luabase->GetBool(1)));
		return 1;
	}

	// meowy.JoinBoostState() -> "on" / "off": what Windows has for the game now.
	int JoinBoostState(lua_State* L)
	{
		using GetInfoFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
		static const GetInfoFn get = reinterpret_cast<GetInfoFn>(
			GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessInformation"));
		PROCESS_POWER_THROTTLING_STATE s = {};
		s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		const bool ok = get && get(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
		L->luabase->PushString(!ok ? "unknown" : (s.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) && !(s.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) ? "on" : "off");
		return 1;
	}

	// In game: keep the list for this server (ours or public, the engine's own
	// address). -> files kept.
	int PrewarmEndJoin(lua_State* L)
	{
		std::string address;
		const server_gate::Result r = server_gate::Check(&address);
		const bool connected = r == server_gate::Result::Allowed || r == server_gate::Result::OtherServer;
		const int kept = prewarm::EndJoin(DataDir(), connected && RejoinableAddress(address) ? address : std::string());
		L->luabase->PushNumber(kept);
		return 1;
	}

	// PrewarmRecorded(address) -> true, MB | false.
	int PrewarmRecorded(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* a = LUA->IsType(1, Type::String) ? LUA->GetString(1) : nullptr;
		uint64_t bytes = 0;
		if (!a || !RejoinableAddress(a) || !prewarm::Recorded(DataDir(), a, &bytes))
		{
			LUA->PushBool(false);
			return 1;
		}
		LUA->PushBool(true);
		LUA->PushNumber(static_cast<double>(bytes) / 1048576.0);
		return 2;
	}

	// PrewarmStart(address) -> true | false, reason.
	int PrewarmStart(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* a = LUA->IsType(1, Type::String) ? LUA->GetString(1) : nullptr;
		std::string why = "not a server address";
		const bool ok = a && RejoinableAddress(a) && prewarm::Start(DataDir(), a, kPrewarmBudget, why);
		LUA->PushBool(ok);
		if (ok)
			return 1;
		LUA->PushString(why.c_str());
		return 2;
	}

	// PrewarmWorkshop(address) -> { "<id>", ... }: the Workshop items its last
	// join mounted (as strings: steamworks takes IDs as strings), possibly empty.
	int PrewarmWorkshop(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* a = LUA->IsType(1, Type::String) ? LUA->GetString(1) : nullptr;
		std::vector<uint64_t> ids;
		if (a && RejoinableAddress(a))
			ids = prewarm::WorkshopItems(DataDir(), a);
		LUA->CreateTable();
		int i = 0;
		for (uint64_t id : ids)
		{
			LUA->PushNumber(++i);
			LUA->PushString(std::to_string(static_cast<unsigned long long>(id)).c_str());
			LUA->SetTable(-3);
		}
		return 1;
	}

	// A Workshop ID passed as a decimal string (steamworks' way), or 0.
	uint64_t WorkshopIdArg(ILuaBase* LUA, int at)
	{
		const char* s = LUA->IsType(at, Type::String) ? LUA->GetString(at) : nullptr;
		uint64_t id = 0;
		for (const char* p = s; p && *p; ++p)
		{
			if (*p < '0' || *p > '9' || id > 1000000000000000000ull)
				return 0;
			id = id * 10 + static_cast<uint64_t>(*p - '0');
		}
		return id;
	}

	// WorkshopItemState(id) -> installed, needsUpdate, busy, legacy, updated |
	// nil: what Steam knows about a Workshop item here (steam_ugc.h). Read-only.
	int WorkshopItemState(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const uint64_t id = WorkshopIdArg(LUA, 1);
		steam_ugc::ItemState st;
		if (!id || !steam_ugc::State(id, st))
		{
			LUA->PushNil();
			return 1;
		}
		LUA->PushBool(st.installed);
		LUA->PushBool(st.needsUpdate);
		LUA->PushBool(st.busy);
		LUA->PushBool(st.legacy);
		LUA->PushNumber(static_cast<double>(st.updated));
		return 5;
	}

	// WorkshopItemProgress(id) -> bytes done, bytes in all | nil: a download's
	// progress, as Steam reports it (steam_ugc.h). Read-only.
	int WorkshopItemProgress(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const uint64_t id = WorkshopIdArg(LUA, 1);
		uint64_t done = 0, total = 0;
		if (!id || !steam_ugc::Progress(id, done, total))
		{
			LUA->PushNil();
			return 1;
		}
		LUA->PushNumber(static_cast<double>(done));
		LUA->PushNumber(static_cast<double>(total));
		return 2;
	}

	// --- Content platform (content_advert.h, docs/CONTENT_PLATFORM.md) ------------
	// ContentAsk(address) -> true | false: read a public server's `meowy_content`
	// rule (A2S_RULES) on a worker thread.
	int ContentAsk(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		LUA->PushBool(LUA->IsType(1, Type::String) && content_advert::Ask(LUA->GetString(1)));
		return 1;
	}

	// ContentTake() -> { { address =, answered = bool, value = string | nil }, ... }:
	// the answers since the last call. `value` is the server's own text
	// (printable ASCII, at most 4 KB); content_logic.lua checks it.
	int ContentTake(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const std::vector<content_advert::Answer> answers = content_advert::Take();
		LUA->CreateTable();
		int i = 0;
		for (const content_advert::Answer& a : answers)
		{
			LUA->PushNumber(++i);
			LUA->CreateTable();
			LUA->PushString(a.address.c_str());
			LUA->SetField(-2, "address");
			LUA->PushBool(a.answered);
			LUA->SetField(-2, "answered");
			if (a.found)
			{
				LUA->PushString(a.value.c_str(), static_cast<unsigned int>(a.value.size()));
				LUA->SetField(-2, "value");
			}
			LUA->SetTable(-3);
		}
		return 1;
	}

	int ContentState(lua_State* L)
	{
		L->luabase->PushString(content_advert::Describe().c_str());
		return 1;
	}

	int PrewarmCancel(lua_State*)
	{
		prewarm::Cancel();
		return 0;
	}

	// --- The console's lines (console_capture.h, U8) -------------------------------
	// ConsoleCaptureStart() -> true | false, reason.
	int ConsoleCaptureStart(lua_State* L)
	{
		std::string why;
		const bool ok = console_capture::Install(why);
		L->luabase->PushBool(ok);
		if (ok)
			return 1;
		L->luabase->PushString(why.c_str());
		return 2;
	}

	// ConsoleRead(afterId, max) -> { {id, t, s, c, x}, ... }, lastId: the lines
	// after `afterId` (the newest `max` when there are more), and the id to pass
	// next time. t: unix ms; s: severity; c: 0xRRGGBB or -1; x: the text.
	int ConsoleRead(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const double after = LUA->IsType(1, Type::Number) ? LUA->GetNumber(1) : 0;
		const double max = LUA->IsType(2, Type::Number) ? LUA->GetNumber(2) : 1000;
		uint64_t next = 1;
		const std::vector<console_capture::Line> lines = console_capture::Read(after > 0 ? static_cast<uint64_t>(after) : 0,
			max < 1 ? 1 : max > 50000 ? 50000 : static_cast<size_t>(max), &next);
		LUA->CreateTable();
		int i = 0;
		for (const console_capture::Line& l : lines)
		{
			LUA->PushNumber(++i);
			LUA->CreateTable();
			LUA->PushNumber(static_cast<double>(l.id));
			LUA->SetField(-2, "id");
			LUA->PushNumber(static_cast<double>(l.timeMs));
			LUA->SetField(-2, "t");
			LUA->PushNumber(l.severity);
			LUA->SetField(-2, "s");
			LUA->PushNumber(l.color == 0xFFFFFFFF ? -1.0 : static_cast<double>(l.color));
			LUA->SetField(-2, "c");
			LUA->PushString(l.text.c_str(), static_cast<unsigned int>(l.text.size()));
			LUA->SetField(-2, "x");
			LUA->SetTable(-3);
		}
		LUA->PushNumber(static_cast<double>(next - 1));
		return 2;
	}

	int ConsoleState(lua_State* L)
	{
		L->luabase->PushString(console_capture::Describe().c_str());
		return 1;
	}

	// The console's saved lists: ConsoleFileRead(kind) -> text, ConsoleFileWrite(kind,
	// text) -> ok, for kind "history" or "pins" only, in
	// %LOCALAPPDATA%\MeowyClientPack\console_<kind>.txt (64 KB at most).
	constexpr size_t kMaxConsoleFile = 64 * 1024;
	const wchar_t* ConsoleFileName(const char* kind)
	{
		if (!kind)
			return nullptr;
		if (std::strcmp(kind, "history") == 0)
			return L"\\console_history.txt";
		if (std::strcmp(kind, "pins") == 0)
			return L"\\console_pins.txt";
		return nullptr;
	}

	int ConsoleFileRead(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const wchar_t* name = ConsoleFileName(LUA->IsType(1, Type::String) ? LUA->GetString(1) : nullptr);
		const std::string text = name ? file_util::ReadSmall(DataDir() + name, kMaxConsoleFile) : std::string();
		LUA->PushString(text.c_str(), static_cast<unsigned int>(text.size()));
		return 1;
	}

	int ConsoleFileWrite(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const wchar_t* name = ConsoleFileName(LUA->IsType(1, Type::String) ? LUA->GetString(1) : nullptr);
		unsigned int size = 0;
		const char* text = LUA->IsType(2, Type::String) ? LUA->GetString(2, &size) : nullptr;
		LUA->PushBool(name && text && size <= kMaxConsoleFile && file_util::WriteAtomic(DataDir() + name, std::string(text, size)));
		return 1;
	}

	// MaskPaths(text) -> text with absolute paths (which carry the Windows user
	// name) replaced by "<path>": crash_reports.h's masker, for the console's
	// error reports.
	int MaskPathsLua(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		unsigned int size = 0;
		const char* text = LUA->IsType(1, Type::String) ? LUA->GetString(1, &size) : "";
		const std::string masked = crashes::MaskPaths(std::string(text, size));
		LUA->PushString(masked.c_str(), static_cast<unsigned int>(masked.size()));
		return 1;
	}

	// ConsoleSaveLog(text) -> path | nil: the console's lines, paths masked, in
	// %LOCALAPPDATA%\MeowyClientPack\logs\console_YYYY-MM-DD_HH-MM-SS.txt (8 MB at
	// most; the newest 20 kept). The path returned is the file's real one.
	int ConsoleSaveLog(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		unsigned int size = 0;
		const char* text = LUA->IsType(1, Type::String) ? LUA->GetString(1, &size) : nullptr;
		if (!text || size > 8u * 1024 * 1024)
		{
			LUA->PushNil();
			return 1;
		}
		const std::wstring dir = DataDir() + L"\\logs";
		CreateDirectoryW(dir.c_str(), nullptr);
		SYSTEMTIME st;
		GetLocalTime(&st);
		wchar_t name[64];
		swprintf_s(name, L"\\console_%04u-%02u-%02u_%02u-%02u-%02u.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		const std::wstring path = dir + name;
		if (!file_util::WriteAtomic(path, crashes::MaskPaths(std::string(text, size))))
		{
			LUA->PushNil();
			return 1;
		}
		// The newest 20 kept (the names sort by time).
		std::vector<std::wstring> logs;
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((dir + L"\\console_*.txt").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE)
		{
			do logs.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		std::sort(logs.begin(), logs.end());
		for (size_t i = 0; i + 20 < logs.size(); ++i)
			DeleteFileW((dir + L"\\" + logs[i]).c_str());
		// The file's own path, unmasked: it goes to the player's clipboard and the
		// console's note (menu Lua only), never off this PC. Masked, it was all "<path>".
		const int n = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string utf8(n > 0 ? n - 1 : 0, '\0');
		if (n > 1)
			WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, &utf8[0], n, nullptr, nullptr);
		LUA->PushString(utf8.c_str(), static_cast<unsigned int>(utf8.size()));
		return 1;
	}

	// ConsoleRun(line) -> ok: the line run as if typed into the console (the
	// player's own line, from the pack's console window).
	int ConsoleRun(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* line = LUA->IsType(1, Type::String) ? LUA->GetString(1) : nullptr;
		LUA->PushBool(console_capture::RunLine(line));
		return 1;
	}

	// --- Server lists from Steam (server_query.h, U6 N1) ----------------------------
	// ServerQueryStart("internet"|"friends", key1, value1, key2, value2, ...) ->
	// id | nil, reason. The filters are Steam's master-server keys (checked).
	int ServerQueryStart(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* list = LUA->IsType(1, Type::String) ? LUA->GetString(1) : "";
		server_query::Filters filters;
		const int top = LUA->Top();
		for (int i = 2; i + 1 <= top && filters.size() < 17; i += 2)
		{
			if (!LUA->IsType(i, Type::String) || !LUA->IsType(i + 1, Type::String))
				break;
			filters.emplace_back(LUA->GetString(i), LUA->GetString(i + 1));
		}
		std::string why = "unknown list";
		const bool friends = std::strcmp(list, "friends") == 0;
		const int id = (friends || std::strcmp(list, "internet") == 0)
			? server_query::Start(friends ? server_query::List::Friends : server_query::List::Internet, filters, why)
			: 0;
		if (id)
		{
			LUA->PushNumber(id);
			return 1;
		}
		LUA->PushNil();
		LUA->PushString(why.c_str());
		return 2;
	}

	// ServerQueryTake(id) -> { {address, name, map, gamedir, desc, tags, ping,
	// players, max, bots, pass, secure, anon, lastplayed, version}, ... },
	// finished, failed. version is the network version (the tags' ver: is the build).
	// Server text is the server's own: shown as text only.
	int ServerQueryTake(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const int id = LUA->IsType(1, Type::Number) ? static_cast<int>(LUA->GetNumber(1)) : 0;
		bool finished = true;
		int failed = 0;
		const std::vector<server_query::Server> servers = server_query::Take(id, &finished, &failed);
		LUA->CreateTable();
		int i = 0;
		for (const server_query::Server& s : servers)
		{
			LUA->PushNumber(++i);
			LUA->CreateTable();
			const auto str = [&](const char* k, const std::string& v) {
				LUA->PushString(v.c_str(), static_cast<unsigned int>(v.size()));
				LUA->SetField(-2, k);
			};
			const auto num = [&](const char* k, double v) { LUA->PushNumber(v); LUA->SetField(-2, k); };
			str("address", s.address);
			str("name", s.name);
			str("map", s.map);
			str("gamedir", s.gameDir);
			str("desc", s.description);
			str("tags", s.tags);
			num("ping", s.ping);
			num("players", s.players);
			num("max", s.maxPlayers);
			num("bots", s.bots);
			LUA->PushBool(s.password);
			LUA->SetField(-2, "pass");
			LUA->PushBool(s.secure);
			LUA->SetField(-2, "secure");
			LUA->PushBool(s.anonymous);
			LUA->SetField(-2, "anon");
			num("lastplayed", s.lastPlayed);
			num("version", s.version);
			LUA->SetTable(-3);
		}
		LUA->PushBool(finished);
		LUA->PushNumber(failed);
		return 3;
	}

	int ServerQueryCancel(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		if (LUA->IsType(1, Type::Number))
			server_query::Cancel(static_cast<int>(LUA->GetNumber(1)));
		return 0;
	}

	int ServerQueryState(lua_State* L)
	{
		L->luabase->PushString(server_query::Describe().c_str());
		return 1;
	}

	// --- Meowy's server index (server_index.h, MR3) ---------------------------------
	// ServerIndexFetch() -> true when a fetch started (false: one is running).
	int ServerIndexFetch(lua_State* L)
	{
		L->luabase->PushBool(server_index::Fetch());
		return 1;
	}

	// ServerIndexTake() -> nil while nothing finished; else status ("ok", "same",
	// "error: ..."), body (JSON for "ok", else ""), geo ("lat,lon" or ""), seconds.
	// The body is the backend's; menu_page.lua checks every row it uses.
	int ServerIndexTake(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		server_index::Result r = server_index::Take();
		if (!r.ready)
		{
			LUA->PushNil();
			return 1;
		}
		LUA->PushString(r.status.c_str(), static_cast<unsigned int>(r.status.size()));
		LUA->PushString(r.body.c_str(), static_cast<unsigned int>(r.body.size()));
		LUA->PushString(r.geo.c_str(), static_cast<unsigned int>(r.geo.size()));
		LUA->PushNumber(r.seconds);
		return 4;
	}

	int ServerIndexState(lua_State* L)
	{
		L->luabase->PushString(server_index::Describe().c_str());
		return 1;
	}

	// PingMemoryLoad() -> the ping memory's JSON, or "" (server_index.h: kept out
	// of garrysmod/data and the game's cookies, which any server's Lua can read).
	int PingMemoryLoad(lua_State* L)
	{
		const std::string json = server_index::LoadPings(DataDir());
		L->luabase->PushString(json.c_str(), static_cast<unsigned int>(json.size()));
		return 1;
	}

	// PingMemorySave(json) -> true | false
	int PingMemorySave(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		LUA->PushBool(LUA->IsType(1, Type::String) && server_index::SavePings(DataDir(), LUA->GetString(1)));
		return 1;
	}

	// --- Parties (party_steam.h, MR3) --------------------------------------------------
	// SteamIDs go to and from Lua as decimal strings (a Lua number can't hold them).
	uint64_t SteamIdArg(ILuaBase* LUA, int i)
	{
		if (!LUA->IsType(i, Type::String))
			return 0;
		const char* s = LUA->GetString(i);
		if (!s || !*s || std::strlen(s) > 20)
			return 0;
		for (const char* p = s; *p; ++p)
			if (*p < '0' || *p > '9')
				return 0;
		return std::strtoull(s, nullptr, 10);
	}

	void PushId(ILuaBase* LUA, uint64_t id)
	{
		const std::string s = std::to_string(id);
		LUA->PushString(s.c_str(), static_cast<unsigned int>(s.size()));
	}

	// PartyAvailable() -> true | false, why
	int PartyAvailable(lua_State* L)
	{
		const std::string why = party_steam::Unavailable();
		L->luabase->PushBool(why.empty());
		L->luabase->PushString(why.c_str());
		return 2;
	}

	int PartyStart(lua_State*)
	{
		party_steam::Start();
		return 0;
	}

	int PartyCreate(lua_State* L)
	{
		L->luabase->PushBool(party_steam::Create(8));
		return 1;
	}

	// PartyJoin(lobbyId) -> true | false (only a lobby's SteamID)
	int PartyJoin(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const uint64_t lobby = LUA->IsType(1, Type::String) ? party_steam::detail::ParseLobbyId(LUA->GetString(1)) : 0;
		LUA->PushBool(lobby && party_steam::Join(lobby));
		return 1;
	}

	int PartyLeave(lua_State*)
	{
		party_steam::Leave();
		return 0;
	}

	int PartyInviteDialog(lua_State* L)
	{
		L->luabase->PushBool(party_steam::OpenInviteDialog());
		return 1;
	}

	// PartyInvite(friendId) -> true | false
	int PartyInvite(lua_State* L)
	{
		L->luabase->PushBool(party_steam::Invite(SteamIdArg(L->luabase, 1)));
		return 1;
	}

	// PartyInviteToServer(friendId, "a.b.c.d:port") -> true | false: the stock
	// "join game" invite, for a friend without the pack.
	int PartyInviteToServer(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* address = LUA->IsType(2, Type::String) ? LUA->GetString(2) : "";
		LUA->PushBool(party_steam::InviteToServer(SteamIdArg(LUA, 1), address));
		return 1;
	}

	// PartySet("lobby" | "member" | "presence", key, value) -> true | false.
	// Only the keys the party uses: lobby srv, srvname, go; member at, atname,
	// pack; presence steam_player_group, steam_player_group_size, meowy_pack.
	int PartySet(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* kind = LUA->IsType(1, Type::String) ? LUA->GetString(1) : "";
		const char* key = LUA->IsType(2, Type::String) ? LUA->GetString(2) : "";
		const std::string value = LUA->IsType(3, Type::String) ? LUA->GetString(3) : "";
		const auto is = [&](const char* a) { return std::strcmp(key, a) == 0; };
		bool ok = false;
		if (std::strcmp(kind, "lobby") == 0 && (is("srv") || is("srvname") || is("go")))
			ok = party_steam::SetLobbyData(key, value);
		else if (std::strcmp(kind, "member") == 0 && (is("at") || is("atname") || is("pack")))
			ok = party_steam::SetMemberData(key, value);
		else if (std::strcmp(kind, "presence") == 0 && (is("steam_player_group") || is("steam_player_group_size") || is("meowy_pack")))
			ok = party_steam::SetPresence(key, value);
		LUA->PushBool(ok);
		return 1;
	}

	// PartyState() -> nil (no party) or { lobby, owner, self, srv, srvname, go,
	// members = { { id, name, at, atname, pack }, ... } }. Names and data are
	// other people's: shown as text only.
	int PartyState(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const party_steam::State s = party_steam::Read();
		if (!s.lobby)
		{
			LUA->PushNil();
			return 1;
		}
		const auto str = [&](const char* k, const std::string& v) {
			LUA->PushString(v.c_str(), static_cast<unsigned int>(v.size()));
			LUA->SetField(-2, k);
		};
		const auto id = [&](const char* k, uint64_t v) { PushId(LUA, v); LUA->SetField(-2, k); };
		LUA->CreateTable();
		id("lobby", s.lobby);
		id("owner", s.owner);
		id("self", s.self);
		str("srv", s.srv);
		str("srvname", s.srvName);
		str("go", s.go);
		LUA->CreateTable();
		int n = 0;
		for (const party_steam::Member& m : s.members)
		{
			LUA->PushNumber(++n);
			LUA->CreateTable();
			id("id", m.id);
			str("name", m.name);
			str("at", m.at);
			str("atname", m.atName);
			str("pack", m.pack);
			LUA->SetTable(-3);
		}
		LUA->SetField(-2, "members");
		return 1;
	}

	// PartyPoll() -> { "created" | "joined" | "failed: ..." | "invite <lobby id>", ... }
	int PartyPoll(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const std::vector<std::string> events = party_steam::Poll();
		LUA->CreateTable();
		int n = 0;
		for (const std::string& e : events)
		{
			LUA->PushNumber(++n);
			LUA->PushString(e.c_str(), static_cast<unsigned int>(e.size()));
			LUA->SetTable(-3);
		}
		return 1;
	}

	// PartyFriends() -> { { id, name, ingmod, pack }, ... }: Steam friends, those
	// playing Garry's Mod first (and whether their pack says so), at most 200.
	int PartyFriends(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const std::vector<party_steam::Friend> friends = party_steam::Friends(200);
		LUA->CreateTable();
		int n = 0;
		for (const party_steam::Friend& f : friends)
		{
			LUA->PushNumber(++n);
			LUA->CreateTable();
			PushId(LUA, f.id);
			LUA->SetField(-2, "id");
			LUA->PushString(f.name.c_str(), static_cast<unsigned int>(f.name.size()));
			LUA->SetField(-2, "name");
			LUA->PushBool(f.inGmod);
			LUA->SetField(-2, "ingmod");
			LUA->PushBool(f.hasPack);
			LUA->SetField(-2, "pack");
			LUA->SetTable(-3);
		}
		return 1;
	}

	int PartyStatus(lua_State* L)
	{
		L->luabase->PushString(party_steam::Describe().c_str());
		return 1;
	}

	int PrewarmState(lua_State* L)
	{
		L->luabase->PushString(prewarm::Describe().c_str());
		return 1;
	}

	// meowy.PakCache(on) -> true | false, reason (pak_cache.h). On while a join
	// loads (patched in on the first); off empties it.
	int PakCache(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const bool on = LUA->GetBool(1);
		if (on)
		{
			std::string why;
			if (!pak_cache::Install(why))
			{
				LUA->PushBool(false);
				LUA->PushString(why.c_str());
				return 2;
			}
		}
		pak_cache::SetActive(on);
		LUA->PushBool(true);
		return 1;
	}

	int PakCacheState(lua_State* L)
	{
		L->luabase->PushString(pak_cache::Describe().c_str());
		return 1;
	}

	// --- The signed policy: Meowy Network's kill switch (policy.h) -------------------
	std::wstring PolicyDir() { return DataDir() + L"\\state"; }

	// Whether this version may run under the policy kept (or the default: all on).
	bool PolicyAllows()
	{
		return policy::VersionAllowed(policy::Effective(), VERSION);
	}

	// The line printed when it may not.
	std::string PolicyOffLine()
	{
		const policy::Policy p = policy::Effective();
		std::string s = std::string("[Meowy Client Pack] ") + VERSION + (p.packEnabled ? " has been paused by Meowy Network" : " is switched off by Meowy Network")
			+ " (policy " + std::to_string(p.seq) + "); the game runs as it would without it";
		if (!p.notice.empty())
			s += ": " + p.notice;
		return s;
	}

	// --- The signed updater (updater.h) ------------------------------------------------
	// The start-up stub's version (meowy_stub_version; 0 for a stub from before updates).
	int StubVersion()
	{
		HMODULE stub = GetModuleHandleW(sizeof(void*) == 8 ? L"gmsv_meowy_boot_win64.dll" : L"gmsv_meowy_boot_win32.dll");
		using Fn = int (*)();
		Fn f = stub ? reinterpret_cast<Fn>(GetProcAddress(stub, "meowy_stub_version")) : nullptr;
		return f ? f() : 0;
	}

	// meowy.UpdaterStart(): check for signed updates (meowy_updates on).
	int UpdaterStart(lua_State*)
	{
		updater::Start(DataDir(), VERSION, StubVersion());
		return 0;
	}

	int UpdaterState(lua_State* L)
	{
		L->luabase->PushString(updater::Describe().c_str());
		return 1;
	}

	// meowy.UpdaterReady() -> the version staged this session, once; else nil.
	int UpdaterReady(lua_State* L)
	{
		const std::string v = updater::TakeReadyNotice();
		if (v.empty())
			L->luabase->PushNil();
		else
			L->luabase->PushString(v.c_str());
		return 1;
	}

	// meowy.PolicyState() -> { seq, allowed, pack_enabled, off = { setting names }, notice, describe }.
	int PolicyState(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const policy::Policy p = policy::Effective();
		LUA->CreateTable();
		LUA->PushNumber(static_cast<double>(p.seq));
		LUA->SetField(-2, "seq");
		LUA->PushBool(policy::VersionAllowed(p, VERSION));
		LUA->SetField(-2, "allowed");
		LUA->PushBool(p.packEnabled);
		LUA->SetField(-2, "pack_enabled");
		LUA->CreateTable();
		double i = 0;
		for (const std::string& name : p.off)
		{
			LUA->PushNumber(++i);
			LUA->PushString(name.c_str());
			LUA->SetTable(-3);
		}
		LUA->SetField(-2, "off");
		LUA->PushString(p.notice.c_str());
		LUA->SetField(-2, "notice");
		LUA->PushString(policy::Describe().c_str());
		LUA->SetField(-2, "describe");
		return 1;
	}

	void PrintLine(ILuaBase* LUA, const std::string& line)
	{
		LUA->PushSpecial(SPECIAL_GLOB);
		LUA->GetField(-1, "print");
		if (LUA->IsType(-1, Type::Function))
		{
			LUA->PushString(line.c_str());
			LUA->Call(1, 0);
		}
		else
		{
			LUA->Pop();
		}
		LUA->Pop();
	}

	// --- Copies of config.cfg (config_backup.h), menu only ---------------------------
	std::wstring ConfigBackupDir() { return DataDir() + L"\\config_backups"; }

	// meowy.ConfigBackup() -> true, "copied" | "unchanged"; or false, why. Once per start.
	int ConfigBackup(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		SYSTEMTIME t;
		GetLocalTime(&t);
		char stamp[32];
		std::snprintf(stamp, sizeof stamp, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		std::string result;
		const bool ok = config_backup::Backup(GarrysModDir() + L"\\cfg\\config.cfg", ConfigBackupDir(), stamp, result);
		LUA->PushBool(ok);
		LUA->PushString(result.c_str());
		return 2;
	}

	// meowy.ConfigBackups() -> { { name, when, size } }, newest first.
	int ConfigBackups(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		LUA->CreateTable();
		double i = 0;
		for (const config_backup::Entry& e : config_backup::List(ConfigBackupDir()))
		{
			LUA->PushNumber(++i);
			LUA->CreateTable();
			LUA->PushString(e.name.c_str());
			LUA->SetField(-2, "name");
			LUA->PushString(e.when.c_str());
			LUA->SetField(-2, "when");
			LUA->PushNumber(static_cast<double>(e.size));
			LUA->SetField(-2, "size");
			LUA->SetTable(-3);
		}
		return 1;
	}

	// meowy.ConfigRestore(name) -> true, "meowy_restore.cfg" (for exec); or false, why.
	int ConfigRestore(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		std::string why;
		const bool ok = config_backup::Restore(ConfigBackupDir(), LUA->CheckString(1), GarrysModDir() + L"\\cfg", why);
		LUA->PushBool(ok);
		LUA->PushString(why.c_str());
		return 2;
	}

	// meowy.ConfigRestoreDone() -> ok: meowy_restore.cfg removed from the game's
	// cfg folder (the exec has run, or it was left behind); true when it's gone.
	int ConfigRestoreDone(lua_State* L)
	{
		L->luabase->PushBool(config_backup::RemoveRestore(GarrysModDir() + L"\\cfg"));
		return 1;
	}

	// LastServerMark(name) -> ok: the player is in the game on a server now; kept
	// for the main menu's "Continue" when it's ours or a public one (the engine's
	// address, read here; the name is the server's, only ever shown as text).
	int LastServerMark(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		std::string address;
		const server_gate::Result r = server_gate::Check(&address);
		const bool ok = (r == server_gate::Result::Allowed || r == server_gate::Result::OtherServer) && RejoinableAddress(address);
		if (ok)
			resilience::MarkLastServer(DataDir(), address, LUA->IsType(1, Type::String) ? LUA->GetString(1) : "");
		LUA->PushBool(ok);
		return 1;
	}

	// LastServer() -> address, name, unix time; or nil.
	int LastServer(lua_State* L)
	{
		std::string name;
		long long since = 0;
		const std::string address = resilience::LastServer(DataDir(), &name, &since);
		if (address.empty() || !RejoinableAddress(address))
		{
			L->luabase->PushNil();
			return 1;
		}
		L->luabase->PushString(address.c_str());
		L->luabase->PushString(name.c_str());
		L->luabase->PushNumber(static_cast<double>(since));
		return 3;
	}

	int LastServerForget(lua_State*)
	{
		resilience::ForgetLastServer(DataDir());
		return 0;
	}

	// src/lua/resilience_logic.lua, doctor_logic.lua, doctor.lua and
	// resilience.lua (meowy.rc), as one chunk: the rules become the locals
	// `Logic` and `Doctor`, and doctor.lua's `DoctorUI` is a tab of
	// resilience.lua's settings window.
	std::string ResourceText(int id)
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ResourceText), &self);
		HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
		HGLOBAL data = res ? LoadResource(self, res) : nullptr;
		const char* p = data ? static_cast<const char*>(LockResource(data)) : nullptr;
		DWORD size = res ? SizeofResource(self, res) : 0;
		return p && size ? std::string(p, size) : std::string();
	}

	std::string ResilienceChunk()
	{
		std::string ui = ResourceText(IDR_UI_LUA);   // the menu UI kit, as the local `UI`
		std::string logic = ResourceText(IDR_RESILIENCE_LOGIC_LUA);
		std::string glue = ResourceText(IDR_RESILIENCE_LUA);
		if (ui.empty() || logic.empty() || glue.empty())
			return std::string();
		// The PC doctor (doctor_logic.lua as the local `Doctor`, then doctor.lua,
		// whose local `DoctorUI` becomes a tab of resilience.lua's settings window).
		std::string doctorLogic = ResourceText(IDR_DOCTOR_LOGIC_LUA);
		std::string doctorGlue = ResourceText(IDR_DOCTOR_LUA);
		std::string doctor;
		if (!doctorLogic.empty() && !doctorGlue.empty())
			doctor = "local Doctor = (function()\n" + doctorLogic + "\nend)()\n" + doctorGlue + "\n";
		// The main menu's Home card (home_logic.lua as `Home`, given `Logic`; then
		// home.lua in a function of its own, which sees resilience.lua's locals).
		std::string homeLogic = ResourceText(IDR_HOME_LOGIC_LUA);
		std::string homeGlue = ResourceText(IDR_HOME_LUA);
		// The content platform's rules (content_logic.lua as `Content`), which
		// home.lua uses to prepare a server's Workshop items before a join.
		std::string contentLogic = ResourceText(IDR_CONTENT_LOGIC_LUA);
		std::string home;
		if (!homeLogic.empty() && !homeGlue.empty() && !contentLogic.empty())
		{
			home = "\nlocal Home = (function(...)\n" + homeLogic + "\nend)(Logic);\n"
				"local Content = (function()\n" + contentLogic + "\nend)();\n"
				"(function()\n" + homeGlue + "\nend)();\n";
			// Our own main menu page (preview, menu_page.lua), in a function of its own too,
			// with Find servers' index rules (browse_logic.lua) as its local `BrowseLogic`.
			std::string menuPage = ResourceText(IDR_MENU_PAGE_LUA);
			std::string browseLogic = ResourceText(IDR_BROWSE_LOGIC_LUA);
			if (!menuPage.empty() && !browseLogic.empty())
				home += "(function()\nlocal BrowseLogic = (function()\n" + browseLogic + "\nend)()\n" + menuPage + "\nend)()\n";
			// Parties (party_logic.lua as `PartyLogic`, then party.lua), after the menu
			// page, whose handlers call it. The leading ";": after "end)()" a "(" would call its result.
			std::string partyLogic = ResourceText(IDR_PARTY_LOGIC_LUA);
			std::string party = ResourceText(IDR_PARTY_LUA);
			if (!partyLogic.empty() && !party.empty())
				home += ";(function()\nlocal PartyLogic = (function()\n" + partyLogic + "\nend)()\n" + party + "\nend)()\n";
		}
		// The Meowy console (console_logic.lua as `ConsoleLogic`, then console.lua),
		// in a function of its own that sees resilience.lua's locals.
		std::string consoleLogic = ResourceText(IDR_CONSOLE_LOGIC_LUA);
		std::string consoleGlue = ResourceText(IDR_CONSOLE_LUA);
		if (!consoleLogic.empty() && !consoleGlue.empty())
			// The leading ";": after "end)()" a "(" would call its result.
			home += ";(function()\nlocal ConsoleLogic = (function()\n" + consoleLogic + "\nend)()\n" + consoleGlue + "\nend)()\n";
		// The settings guard (settings_guard_logic.lua as `SettingsGuard`, then
		// settings_guard.lua), likewise.
		std::string guardLogic = ResourceText(IDR_SETTINGS_GUARD_LOGIC_LUA);
		std::string guardGlue = ResourceText(IDR_SETTINGS_GUARD_LUA);
		if (!guardLogic.empty() && !guardGlue.empty())
			home += ";(function()\nlocal SettingsGuard = (function()\n" + guardLogic + "\nend)()\n" + guardGlue + "\nend)()\n";
		return "local UI = (function()\n" + ui + "\nend)()\n"
			"local Logic = (function()\n" + logic + "\nend)()\n" + doctor + glue + home;
	}

	// MenuShellJs() -> the Meowy home screen's script, which menu_page.lua puts
	// into the stock menu page: Find servers (src/menu/browser.js) first, then
	// the home (src/menu/shell.js), which builds it in; "" if missing.
	int MenuShellJs(lua_State* L)
	{
		const std::string shell = ResourceText(IDR_MENU_SHELL_JS);
		const std::string js = shell.empty() ? shell : ResourceText(IDR_MENU_BROWSER_JS) + "\n" + shell;
		L->luabase->PushString(js.c_str(), static_cast<unsigned int>(js.size()));
		return 1;
	}
#endif

	// Runs in the menu state. Honest server list: counts in the server browser
	// checked before they're believed. GMod's browser shows players minus bots
	// and ranks servers by it, so a server that reports clients that aren't
	// there (a rewritten query reply, hidden bots, idle boost accounts) jumps
	// the list. This wraps serverlist.Query: each listed server's player list
	// is sampled twice GAP seconds apart and compared with its advertised count,
	// the backend's verdicts (crawler + other players' reports) are applied, and
	// a flagged server shows its real count with a tag and is ranked by it.
	// The rules match the backend's serverRep/score.js; keep them in step.
	// While the player is on someone else's server, the game's own `status`
	// list (SteamIDs, bots) is reported once per server so the backend can
	// spot boost accounts; meowy_serverlist_reports 0 turns that off.
	const char* const kServerListLua = R"lua(
local meowy = meowy
local enabled = meowy.Setting("meowy_honest_serverlist", "1",
	"Meowy Client Pack: check player counts in the server list (0 = off)")
local reports = meowy.Setting("meowy_serverlist_reports", "1",
	"Meowy Client Pack: tell Meowy Network who was really on other servers you join, to catch fake counts (0 = off)")

local MIN_PLAYERS, GAP, MIN_FAKE, MIN_SHARE, DUPE_TOLERANCE = 10, 45, 5, 0.2, 15
local MAX_CHECKS, PARALLEL, LIST_TIMEOUT, RECHECK = 60, 4, 10, 600 -- the query itself gives up within ~8 s

local rep = {}       -- address -> verdict from the backend
local afk = {}       -- address -> { share, judged, minutes }: the observer saw most of them idle
local mine = {}      -- address -> verdict from this PC's checks (.at = SysTime)
local seen = {}      -- address -> { players, bots, type }
local tagged = {}    -- type .. address -> true once the browser shows a tag
local samples = {}   -- address -> { at = SysTime, list = { {name, time} } }
local waiting = {}   -- address -> { done, deadline }: player lists asked for
local declared = {}  -- address -> the count the last reply's header claimed
local repAt = -1e9

-- The menu state has no timer library: a small one, run from Think.
local later = {}
local function after(seconds, fn) later[#later + 1] = { at = SysTime() + seconds, fn = fn } end
hook.Add("Think", "MeowyClientPack.ServerListTimers", function()
	if #later == 0 then return end
	local now, due = SysTime(), {}
	for i = #later, 1, -1 do
		if later[i].at <= now then due[#due + 1] = later[i].fn table.remove(later, i) end
	end
	-- Each on its own: one that fails mustn't drop the rest (a check left
	-- pending for the session, a status capture never ended).
	for _, fn in ipairs(due) do
		local ok, err = pcall(fn)
		if not ok then ErrorNoHalt("[Meowy Client Pack] server list: " .. tostring(err) .. "\n") end
	end
end)

-- Players' reports (who was really connected) beat everything; then this PC's
-- own fresh check; the crawler's verdict only for servers not checked here.
-- A clean check here doesn't hide what the observer learned by joining: that
-- it saw nothing fake (verified) or that the server refused it (unverified).
local function verdictOf(address)
	local r = rep[address]
	if r and r.source == "players" then return r end
	local m = mine[address]
	if m and SysTime() - m.at < RECHECK then
		if m.verdict ~= "ok" then return m end
		if r and (r.verdict == "verified" or (r.verdict == "unverified" and r.source == "observer")) then return r end
		return nil
	end
	return r
end

local function tagFor(v)
	if v.verdict == "fake" then return "[FAKE - " .. tostring(v.real) .. " real] " end
	if v.verdict == "verified" then return "[VERIFIED] " end -- the observer joined and saw nothing fake
	if v.verdict == "unverified" then
		-- A hidden list is a fact, not an accusation: big servers block the query against floods.
		-- Neither is a server that refuses the observer ("blocks automated checks": VPN/hosting blocks).
		local why = istable(v.reasons) and tostring(v.reasons[1] or "") or ""
		return why:find("hidden", 1, true) and "[LIST HIDDEN] " or "[UNVERIFIED] "
	end
end

-- Unverified servers (either kind) sit a little lower than otherwise-equal
-- ones, never hidden: added to the browser's rank, where lower is better.
local UNVERIFIED_RANK = 40
local function rankPenalty(v)
	return v and v.verdict == "unverified" and UNVERIFIED_RANK or 0
end

-- Separate from the verdicts: plenty of honest DarkRP servers are mostly AFK,
-- and players may want to know. Only from a visit that judged 10+ players.
local AFK_MIN_SHARE, AFK_MIN_JUDGED = 0.5, 10
local function afkTag(address)
	local a = afk[address]
	local share, judged = istable(a) and tonumber(a.share), istable(a) and tonumber(a.judged)
	if not share or not judged or share < AFK_MIN_SHARE or share > 1 or judged < AFK_MIN_JUDGED then return "" end
	return string.format("[~%d%% AFK] ", math.floor(share * 20 + 0.5) * 5)
end

-- The tag a listed server gets (verdict first, then the AFK share), or nil.
local function tagOf(address)
	if meowy.IsOurServer(address) then return nil end
	local v = verdictOf(address)
	local tag = (v and tagFor(v) or "") .. afkTag(address)
	if tag == "" then return nil end
	return tag, v
end

local MARK_JS = [[
if (!window.MeowyMark) window.MeowyMark = function (addr, real, tag, penalty) {
	for (var t in ServerTypes) { var g = ServerTypes[t].gamemodes;
	for (var k in g) { var gm = g[k];
	for (var i = 0; i < gm.servers.length; i++) { var s = gm.servers[i];
		if (s.address != addr || s.meowyTag) continue;
		s.meowyTag = tag; s.name = tag + s.name;
		if (real >= 0 && real < s.players) {
			var d = s.players - real; s.players = real;
			gm.num_players -= d; if (!s.isAnon) gm.sort_players -= d; gm.order = gm.sort_players;
			s.recommended = CalculateRank(s);
		}
		if (penalty > 0 && !s.meowyRankAdj) { s.meowyRankAdj = true; s.recommended += penalty; }
	} } }
	UpdateDigest(RootScope, 50);
};
]]

-- For a server tagged as it arrived: the browser adds it to its list only
-- after the callback returns, so the rank is moved a moment later.
local RANK_JS = [[
if (!window.MeowyRank) window.MeowyRank = function (addr, penalty) {
	var found = false;
	for (var t in ServerTypes) { var g = ServerTypes[t].gamemodes;
	for (var k in g) { var gm = g[k];
	for (var i = 0; i < gm.servers.length; i++) { var s = gm.servers[i];
		if (s.address != addr || s.meowyRankAdj) continue;
		s.meowyRankAdj = true; s.recommended += penalty; found = true;
	} } }
	if (found) UpdateDigest(RootScope, 50);
};
]]

local function rankLater(address, penalty)
	if penalty <= 0 then return end
	local js = RANK_JS .. string.format("MeowyRank('%s', %d);", address:JavascriptSafe(), penalty)
	-- Twice: a busy browser may not have the server yet; the flag stops a double penalty.
	for _, delay in ipairs({ 0.5, 3 }) do
		after(delay, function() if pnlMainMenu then pnlMainMenu:Call(js) end end)
	end
end

-- Shows a verdict that arrived after the server was already listed.
local function mark(address)
	local s = seen[address]
	if not s or not pnlMainMenu or tagged[s.type .. address] then return end
	local tag, v = tagOf(address)
	if not tag then return end
	tagged[s.type .. address] = true
	pnlMainMenu:Call(MARK_JS .. string.format("MeowyMark('%s', %d, '%s', %d);",
		address:JavascriptSafe(), v and v.verdict == "fake" and (v.real or -1) or -1, tag:JavascriptSafe(), rankPenalty(v)))
end

local function fetchRep()
	if SysTime() - repAt < 300 then return end
	repAt = SysTime()
	meowy.ServerRepFetch()
end

local function usable(name) return isstring(name) and #(name:match("^%s*(.-)%s*$")) >= 3 end

-- name -> the moments (SysTime) players of that name joined, for one sample.
-- At most MAX_SAME per name: a list of one name repeated can't make a long one.
local MAX_SAME = 8
local function joinedIndex(list, at)
	local index = {}
	for _, f in ipairs(list) do
		if usable(f.name) then
			local times = index[f.name]
			if not times then times = {} index[f.name] = times end
			if #times < MAX_SAME then times[#times + 1] = at - f.time end
		end
	end
	return index
end

-- The rules of serverRep/score.js, for one server with one or two samples.
local function judge(address, listA, listB, gap)
	local s = seen[address]
	local reported = s.players - s.bots
	if not listA or (#listA == 0 and reported > 0) then
		return { verdict = "unverified", reasons = { "player list hidden" } }
	end
	local suspicious = {}
	if listB and gap > 0 then
		local countA, countB, timeB = {}, {}, {}
		for _, e in ipairs(listA) do countA[e.name] = (countA[e.name] or 0) + 1 end
		for _, e in ipairs(listB) do countB[e.name] = (countB[e.name] or 0) + 1 timeB[e.name] = e.time end
		for _, e in ipairs(listA) do
			if usable(e.name) and countA[e.name] == 1 and countB[e.name] == 1 then
				local grown = timeB[e.name] - e.time
				if grown >= 0 and grown < gap * 0.5 then suspicious[e.name] = true end
			end
		end
	end
	-- Nearly everyone frozen is a cached reply, not fakes (serverRep/score.js).
	local frozen = 0
	for _ in pairs(suspicious) do frozen = frozen + 1 end
	if #listA > 0 and frozen >= #listA * 0.8 then suspicious = {} end
	-- The same name, joined at the same moment, on another server we sampled.
	-- Through each sample's name index (joinedIndex), so the work grows with
	-- the names and servers, not with names times every other server's names.
	for _, e in ipairs(listA) do
		if usable(e.name) and not suspicious[e.name] then
			local joined = samples[address].at - e.time
			for other, o in pairs(samples) do
				local times = other ~= address and samples[address].at - o.at < RECHECK and o.joined[e.name]
				if times then
					for _, t in ipairs(times) do
						if math.abs(t - joined) <= DUPE_TOLERANCE then suspicious[e.name] = true break end
					end
					if suspicious[e.name] then break end
				end
			end
		end
	end
	local n = 0
	for _ in pairs(suspicious) do n = n + 1 end
	local real = math.min(reported, math.max(0, #listA - n))
	local fake = reported - real
	local reasons = { "lists " .. #listA .. " of " .. reported }
	if declared[address] and declared[address] > #listA then
		reasons[#reasons + 1] = "reply says " .. declared[address] .. ", sends " .. #listA
	end
	if fake >= MIN_FAKE and fake >= reported * MIN_SHARE then
		return { verdict = "fake", real = real, reasons = reasons }
	end
	return { verdict = "ok", real = reported }
end

-- (The module already stops at 255 entries and 64-byte names; so does this.)
local MAX_LIST = 255
local function toList(tbl)
	local out = {}
	for _, p in pairs(istable(tbl) and tbl or {}) do
		if #out >= MAX_LIST then break end
		if istable(p) then out[#out + 1] = { name = string.sub(tostring(p.name or ""), 1, 64), time = tonumber(p.time) or 0, score = tonumber(p.score) or 0 } end
	end
	return out
end

-- Check queue: sample A, then B GAP seconds later, then judge.
local queue, running, pending = {}, 0, {}

-- Player lists come from the pack's own query (server_rep.cpp), not
-- serverlist.PlayerList, which shows nothing for a reply that doesn't add up.
-- (waiting and declared are declared above judge(), which reads declared.)

-- The side panel's list for a clicked server, from the same query; GMod's own
-- call is the fallback when ours gets no answer.
local panelWanted = {}
local stockGetPlayerList = GetPlayerList
if isfunction(stockGetPlayerList) then
	GetPlayerList = function(serverip, ...)
		if enabled:GetBool() and isstring(serverip) then
			if meowy.QueryPlayers(serverip) then
				panelWanted[serverip] = true
				return
			end
		end
		return stockGetPlayerList(serverip, ...)
	end
end

local function deliver(address, list)
	local w = waiting[address]
	if not w then return end
	waiting[address] = nil
	running = running - 1
	-- One bad result mustn't stop the rest of the batch (they'd all time out).
	local ok, err = pcall(w.done, list)
	if not ok then ErrorNoHalt("[Meowy Client Pack] server list check: " .. tostring(err) .. "\n") end
end

-- A list is delivered as a table (the reply's entries), nil (the server
-- answered with no list: hidden) or false (no answer at all, or not asked:
-- says nothing, so no verdict).
local function sample(address, done)
	running = running + 1
	waiting[address] = { done = done, deadline = SysTime() + LIST_TIMEOUT }
	if not meowy.QueryPlayers(address) then deliver(address, false) end
end

local function startChecks(type)
	local now = SysTime()
	-- Samples only count against each other while fresh (judge's same-moment
	-- check); older ones go, so the session's list doesn't keep growing.
	for address, smp in pairs(samples) do
		if now - smp.at > RECHECK and not pending[address] then samples[address] = nil end
	end
	local list = {}
	for address, s in pairs(seen) do
		if s.type == type and s.players - s.bots >= MIN_PLAYERS and not meowy.IsOurServer(address)
			and not pending[address] and not (mine[address] and now - mine[address].at < RECHECK) then
			list[#list + 1] = address
		end
	end
	table.sort(list, function(a, b) return seen[a].players > seen[b].players end)
	for i = 1, math.min(#list, MAX_CHECKS) do queue[#queue + 1] = list[i] pending[list[i]] = true end
end

hook.Add("Think", "MeowyClientPack.ServerList", function()
	local json = meowy.ServerRepTake()
	if json then
		local data = util.JSONToTable(json)
		if istable(data) and istable(data.servers) then
			rep = data.servers
			afk = istable(data.afk) and data.afk or {}
			for address in pairs(seen) do mark(address) end
		end
	end
	local results = meowy.TakePlayers()
	if results then
		for _, r in ipairs(util.JSONToTable(results) or {}) do
			if istable(r) and isstring(r.address) then
				if r.ok then declared[r.address] = tonumber(r.declared) end
				if panelWanted[r.address] and not r.ok then
					panelWanted[r.address] = nil
					stockGetPlayerList(r.address)
				elseif panelWanted[r.address] and pnlMainMenu then
					panelWanted[r.address] = nil
					local rows = {}
					for _, p in ipairs(toList(r.players)) do rows[#rows + 1] = { name = p.name, score = tonumber(p.score) or 0, time = p.time } end
					pnlMainMenu:Call("SetPlayerList( '" .. r.address:JavascriptSafe() .. "', " .. util.TableToJSON(rows) .. ")")
				end
				local list = false
				if r.ok then list = toList(r.players) elseif r.answered then list = nil end
				deliver(r.address, list)
			end
		end
	end
	for address, w in pairs(waiting) do
		if SysTime() > w.deadline then deliver(address, false) end
	end
	while running < PARALLEL and #queue > 0 do
		local address = table.remove(queue, 1)
		sample(address, function(listA)
			local atA = SysTime()
			-- No answer: no verdict from here (the backend's, if any, stands).
			if listA == false then pending[address] = nil return end
			if listA then samples[address] = { at = atA, list = listA, joined = joinedIndex(listA, atA) } end
			if not listA or #listA == 0 then
				pending[address] = nil
				mine[address] = judge(address, listA, nil, 0)
				mine[address].at = SysTime()
				mark(address)
				return
			end
			after(GAP, function()
				sample(address, function(listB)
					pending[address] = nil
					if listB == false then listB = nil end -- no second answer: judge on the first alone
					mine[address] = judge(address, listA, listB, SysTime() - atA)
					mine[address].at = SysTime()
					mark(address)
				end)
			end)
		end)
	end
end)

local stockQuery = serverlist.Query
-- The game's own query, for lists that aren't the browser's (the main menu's
-- Home card): no count checks, no backend calls.
meowy.StockServerQuery = stockQuery
-- For the pack's own Find servers (menu_page.lua): a server's tag ([VERIFIED],
-- [FAKE - 12 real], ...) and verdict, as this list marks it, and the verdicts fetched.
meowy.ServerTag = function(address)
	if not enabled:GetBool() or not isstring(address) then return nil end
	return tagOf(address)
end
meowy.FetchServerRep = function() if enabled:GetBool() then fetchRep() end end

-- Find servers, faster (U6 N1, server_query.h): the browser's Internet list
-- straight from Steam, whose master server leaves out empty servers before
-- anything is pinged (Steam pings at most ~5,000 a minute). About a fifth of
-- the ~4,800 servers, complete in ~20 s instead of ~90 s. Each server reaches
-- the browser through the query's own Callback, with the values the game's
-- query gives (its tags read the same way), so the stock page, its blacklist
-- and the count checks above see no difference.
local fastList = meowy.Setting("meowy_fast_serverlist", "1",
	"Meowy Client Pack: Find servers lists only servers with players, straight from Steam (0 = every server, the game's way)")
local fastRuns = {}   -- [list id] = { data, started, count }
local function tagValue(tags, key) return (" " .. tags .. " "):match(" " .. key .. ":(%S+)") end
local function fastQuery(data)
	if not (fastList:GetBool() and meowy.ServerQueryStart and istable(data) and isfunction(data.Callback)) then return false end
	if data.Type ~= "internet" or (data.AppID and data.AppID ~= 4000) or (data.GameDir and data.GameDir ~= "garrysmod") then return false end
	for id in pairs(fastRuns) do meowy.ServerQueryCancel(id) fastRuns[id] = nil end   -- a refresh replaces the last list
	local id = meowy.ServerQueryStart("internet", "gamedir", "garrysmod", "hasplayers", "1")
	if not id then return false end
	fastRuns[id] = { data = data, started = SysTime(), count = 0 }
	return true
end
hook.Add("Think", "MeowyClientPack.FastServerList", function()
	for id, run in pairs(fastRuns) do
		local list, finished = meowy.ServerQueryTake(id)
		local stop = false
		for _, s in ipairs(list) do
			local tags = s.tags or ""
			run.count = run.count + 1
			local ok, more = pcall(run.data.Callback, s.ping, s.name, s.desc, s.map, s.players, s.max, s.bots, s.pass,
				s.lastplayed, s.address, tagValue(tags, "gm") or "", tagValue(tags, "gmws") or "0", s.anon,
				-- netVersion is the network version and luaVersion the build (the tags' ver:), as the
				-- game's own list gives them: the page marks a server outdated by the first.
				tonumber(s.version) or 0, tonumber(tagValue(tags, "ver") or "") or 0, tagValue(tags, "loc") or "", tagValue(tags, "gmc") or "")
			if not ok then ErrorNoHalt("[Meowy Client Pack] fast server list: " .. tostring(more) .. "\n") end
			if not ok or more == false then stop = true break end
		end
		if stop or finished then
			fastRuns[id] = nil
			meowy.ServerQueryCancel(id)
			print(string.format("[Meowy Client Pack] fast server list: %d servers with players in %.1f s%s", run.count,
				SysTime() - run.started, stop and " (stopped)" or ""))
			if finished and not stop and isfunction(run.data.Finished) then run.data.Finished() end
		end
	end
end)
concommand.Add("meowy_fast_serverlist_status", function()
	print("[Meowy Client Pack] fast server list: " .. (fastList:GetBool() and "on" or "off") .. "; " ..
		(meowy.ServerQueryState and meowy.ServerQueryState() or "unavailable"))
end)
serverlist.Query = function(data)
	if enabled:GetBool() and istable(data) and isfunction(data.Callback) then
		local type, callback, finished = data.Type, data.Callback, data.Finished
		for key in pairs(tagged) do if key:sub(1, #tostring(type)) == tostring(type) then tagged[key] = nil end end
		data.Callback = function(ping, name, desc, map, players, maxplayers, botplayers, pass, lastplayed, address, ...)
			seen[address] = { players = players or 0, bots = botplayers or 0, type = tostring(type) }
			local tag, v = tagOf(address)
			if tag then
				tagged[tostring(type) .. address] = true
				name = tag .. tostring(name)
				-- The browser shows players minus bots: count the fakes as bots.
				local fake = v and v.verdict == "fake" and v.real and math.max(0, (players - botplayers) - v.real) or 0
				botplayers, maxplayers = botplayers + fake, maxplayers + fake
				rankLater(address, rankPenalty(v))
			end
			return callback(ping, name, desc, map, players, maxplayers, botplayers, pass, lastplayed, address, ...)
		end
		data.Finished = function(...)
			if finished then finished(...) end
			startChecks(tostring(type))
		end
		fetchRep()
	end
	if fastQuery(data) then return end
	return stockQuery(data)
end

-- SteamID ("STEAM_0:1:23" or "[U:1:47]") to SteamID64, in decimal strings:
-- the 64-bit numbers don't fit a Lua double.
local function addToDecimal(base, n)
	local digits, carry, out = base:reverse(), n, {}
	for i = 1, #digits do
		local d = tonumber(digits:sub(i, i)) + carry % 10
		carry = math.floor(carry / 10) + math.floor(d / 10)
		out[i] = tostring(d % 10)
	end
	while carry > 0 do out[#out + 1] = tostring(carry % 10) carry = math.floor(carry / 10) end
	return table.concat(out):reverse()
end

local function toSteamID64(id)
	local x, y = id:match("^STEAM_%d:(%d):(%d+)$")
	if x then return addToDecimal("76561197960265728", tonumber(y) * 2 + tonumber(x)) end
	local u = id:match("^%[U:1:(%d+)%]$")
	if u then return addToDecimal("76561197960265728", tonumber(u)) end
end

-- One report per server per game session: `status` a while after joining and
-- again RUN_GAP seconds later, so the backend can see pings that don't move and
-- clients that never get past connecting (serverRep/reports.js).
local FIRST_RUN, RUN_GAP = 90, 240

-- "01:22" or "1:02:03" to seconds.
local function toSeconds(s)
	if not isstring(s) then return nil end
	local h, m, sec = s:match("^(%d+):(%d+):(%d+)$")
	if h then return tonumber(h) * 3600 + tonumber(m) * 60 + tonumber(sec) end
	m, sec = s:match("^(%d+):(%d+)$")
	if m then return tonumber(m) * 60 + tonumber(sec) end
end

-- One `status` run: { bots = n, rows = { { id, ping, loss, state, secs } } }.
local function runStatus(done)
	meowy.StatusCaptureBegin()
	RunConsoleCommand("status")
	after(2, function()
		local text = meowy.StatusCaptureEnd()
		local run, seenIds, sawSelf = { bots = 0, rows = {} }, {}, false
		for line in string.gmatch(text, "[^\n]+") do
			local uid, rest = line:match('^#%s*%d+%s+".*"%s+(%S+)%s*(.-)%s*$')
			if uid == "BOT" then
				run.bots = run.bots + 1
			elseif uid then
				local id64 = toSteamID64(uid)
				-- Our own row stays out of the rows. Who sent the report is only in the
			-- Steam ticket native adds (steam_ticket.h), which Meowy Network checks
			-- with Steam and keeps as a salted hash, for one vote per account.
				if id64 and id64 == meowy.SelfSteamID64 then
					sawSelf = true
				elseif id64 and not seenIds[id64] and #run.rows < 128 then
					seenIds[id64] = true
					-- "connected ping loss state [adr]"; a client still connecting may lack the first three.
					local connected, ping, loss, state = rest:match("^(%S+)%s+(%d+)%s+(%d+)%s+(%a+)")
					if not connected then state = rest:match("(%a+)") end
					run.rows[#run.rows + 1] = {
						id = id64, ping = tonumber(ping), loss = tonumber(loss),
						state = string.lower(state or "unknown"):sub(1, 16), secs = toSeconds(connected),
					}
				end
			end
		end
		meowy.ServerRepNote(string.format("status parsed: %d accounts, %d bots", #run.rows, run.bots))
		done(run, sawSelf)
	end)
end

local reported, joinedAt, nextLook, lastAddress = {}, {}, 0, nil
hook.Add("Think", "MeowyClientPack.ServerReport", function()
	if SysTime() < nextLook then return end
	nextLook = SysTime() + 5
	local address = IsInGame() and not IsInLoading() and meowy.OtherServerAddress() or nil
	-- Each visit's FIRST_RUN counts from when it began, a return to a server too.
	if address ~= lastAddress then
		lastAddress = address
		if address then joinedAt[address] = SysTime() end
	end
	if not address or reported[address] or not enabled:GetBool() or not reports:GetBool() then return end
	if SysTime() - joinedAt[address] < FIRST_RUN then return end
	reported[address] = true

	runStatus(function(first, sawSelf)
		-- Without our own row found (our SteamID unknown), nothing is sent: it
		-- could be among the rows.
		if not sawSelf then return meowy.ServerRepNote("report skipped: own row not identified") end
		if #first.rows + 1 + first.bots < MIN_PLAYERS then return end
		after(RUN_GAP, function()
			-- Still on the same server? Otherwise send what the first run saw.
			local still = meowy.OtherServerAddress() == address
			local function send(runs)
				serverlist.PingServer(address, function(ping, name, desc, map, players, maxplayers, botplayers)
					meowy.ServerRepNote("server answered the ping: " .. tostring(players) .. " players, " .. tostring(botplayers) .. " bots")
					if not players then return end
					meowy.ServerRepReport(util.TableToJSON({
						address = address, advertised = players, advertisedBots = botplayers or 0, runs = runs,
					}))
				end)
			end
			if still then runStatus(function(second) send({ first, second }) end) else send({ first }) end
		end)
	end)
end)
)lua";

	// Runs in the menu state. The loading panel is active from the moment a
	// connect starts (IsInLoading); once the engine's connection is to one of
	// our servers, our screen takes over. It only gets fed while the game's
	// main thread runs, but draws on its own thread regardless.
	const char* const kMenuLua = R"lua(
local meowy = meowy

-- The pack's settings (meowy.Setting) come from settings.lua, run just before this.

-- A setting by name, read the way its own chunk reads it (the policy
-- included): meowy.SettingOf(name) -> the same object meowy.Setting returned,
-- or nil. Another chunk reads a setting it doesn't own through this, never
-- GetConVar's raw value. Asking for one twice gives the same object.
do
	local create, byName = meowy.Setting, {}
	function meowy.Setting(name, default, help)
		if byName[name] == nil then byName[name] = create(name, default, help) end
		return byName[name]
	end
	function meowy.SettingOf(name) return byName[name] end
end

-- Signed updates (updater.h): checked a minute after start and every 6 hours;
-- a new version is downloaded, checked against Meowy Network's signature, and
-- starts the next time the game opens (never mid-session).
local updates = meowy.Setting("meowy_updates", "1",
	"Meowy Client Pack: get new versions of the pack, signed by Meowy Network; one starts the next time you open the game (0 = off)")
local updaterAsked, nextUpdateCheck = false, 0
hook.Add("Think", "MeowyClientPack.Updates", function()
	local now = SysTime()
	if now < nextUpdateCheck then return end
	nextUpdateCheck = now + 10
	if not updaterAsked and updates:GetBool() then
		updaterAsked = true
		meowy.UpdaterStart()
	end
	local ready = meowy.UpdaterReady()
	if ready then print("[Meowy Client Pack] Client Pack " .. ready .. " is ready; it starts the next time you open Garry's Mod") end
end)
concommand.Add("meowy_update_status", function()
	print("[Meowy Client Pack] updates: " .. meowy.UpdaterState())
end)

local enabled = meowy.Setting("meowy_loadingscreen", "1",
	"Meowy Client Pack: our own loading screen when joining Meowy Network (0 = Garry's Mod's)")
-- Other servers get the same screen, neutral (loading_screen.h BeginOther),
-- unless they show a loading page of their own: theirs stays.
local everywhere = meowy.Setting("meowy_loadingscreen_everywhere", "1",
	"Meowy Client Pack: the pack's loading screen on other servers too: 1 = on servers without a loading page of their own, 2 = on every server, 0 = only on Meowy Network")
local active, sure, lastStatus, details = false, false, nil, nil

-- The screen's page is loaded ahead (hidden, so a join shows it at once) and
-- unloaded while you play or the game sits in the background: home.lua.
local panelSince -- SysTime when GMod's loading panel opened, for how long it showed before ours

-- A separate garrysmod/data for each server (data_sandbox.h): a server's Lua
-- sees only the files it wrote itself, never other servers' or the menu's.
local sandbox = meowy.Setting("meowy_data_sandbox", "1",
	"Meowy Client Pack: give every server its own garrysmod/data (0 = one data folder for all)")
hook.Add("Think", "MeowyClientPack.DataSandbox", function()
	meowy.DataSandbox(sandbox:GetBool() and (IsInLoading() or IsInGame()))
end)
concommand.Add("meowy_data_sandbox_status", function()
	print("[Meowy Client Pack] data sandbox: " .. meowy.DataSandboxDescribe())
end)

-- Client Lua files are found through the string table's own index instead of
-- a full scan per include and search path (lua_lookup.h): the same answers,
-- on any server. Switched only between servers, never while one is loading.
local fastLookup = meowy.Setting("meowy_fast_lua_lookup", "1",
	"Meowy Client Pack: find a server's Lua files through the string table's index (0 = the game's own scan)")
local fastLookupApplied, wasJoining
hook.Add("Think", "MeowyClientPack.FastLuaLookup", function()
	local joining = IsInLoading() or IsInGame()
	-- Each join's table is checked again before it's used (lua_lookup.h).
	if joining and not wasJoining then meowy.FastLuaLookupNewJoin() end
	wasJoining = joining
	local want = fastLookup:GetBool()
	if want == fastLookupApplied or joining then return end
	fastLookupApplied = want
	local ok, why = meowy.FastLuaLookup(want)
	if not ok then print("[Meowy Client Pack] fast Lua lookup: " .. tostring(why)) end
end)
concommand.Add("meowy_fast_lua_lookup_status", function()
	print("[Meowy Client Pack] fast Lua lookup: " .. meowy.FastLuaLookupState())
end)

-- file.Find on "LUA" walks only the string table entries that can match the
-- pattern instead of copying and testing all of them (lua_find.h): the same
-- answers, on any server. Re-indexed at each join (FastLuaLookupNewJoin above).
local fastFind = meowy.Setting("meowy_fast_lua_find", "1",
	"Meowy Client Pack: let file.Find on a server's Lua skip the files that can't match (0 = the game's own walk)")
local fastFindApplied
hook.Add("Think", "MeowyClientPack.FastLuaFind", function()
	local want = fastFind:GetBool()
	if want == fastFindApplied or IsInLoading() or IsInGame() then return end
	fastFindApplied = want
	local ok, why = meowy.FastLuaFind(want)
	if not ok then print("[Meowy Client Pack] fast file.Find: " .. tostring(why)) end
end)
concommand.Add("meowy_fast_lua_find_status", function()
	print("[Meowy Client Pack] fast file.Find: " .. meowy.FastLuaFindState())
end)

-- At every map load the game registers all ~400 custom font files again, and
-- loads and frees gdi32.dll for each one; the gdi32 that's already loaded is
-- used instead (font_loads.h).
local fastFonts = meowy.Setting("meowy_fast_fonts", "1",
	"Meowy Client Pack: don't load gdi32.dll again for every font file on each join (0 = the game's own way)")
local fastFontsApplied
hook.Add("Think", "MeowyClientPack.FastFonts", function()
	local want = fastFonts:GetBool()
	if want == fastFontsApplied or IsInLoading() or IsInGame() then return end
	fastFontsApplied = want
	local ok, why = meowy.FastFonts(want)
	if not ok then print("[Meowy Client Pack] fast fonts: " .. tostring(why)) end
end)
concommand.Add("meowy_fast_fonts_status", function()
	print("[Meowy Client Pack] fast fonts: " .. meowy.FastFontsState())
end)

-- While you aim (the cursor hidden, the game in front), the mouse is held
-- inside the game's window, so a fast flick can't click another window or
-- monitor and drop the game to the background; menus and alt-tab free it at
-- once (mouse_confine.h).
local mouseConfine = meowy.Setting("meowy_mouse_confine", "1",
	"Meowy Client Pack: while you aim, keep the mouse inside the game so a fast flick can't click another window or monitor (0 = off)")
local mouseConfineApplied
hook.Add("Think", "MeowyClientPack.MouseConfine", function()
	local want = mouseConfine:GetBool()
	if want == mouseConfineApplied then return end
	mouseConfineApplied = want
	meowy.MouseConfine(want)
end)
concommand.Add("meowy_mouse_confine_status", function()
	print("[Meowy Client Pack] mouse confine: " .. meowy.MouseConfineState())
end)

-- While a join loads, the game looks for each model and material in every
-- search folder in turn; a folder already read once answers "not there"
-- without trying to open the file (fs_cache.h). Installed between servers,
-- active only while a join loads.
local fastFiles = meowy.Setting("meowy_fast_file_checks", "1",
	"Meowy Client Pack: answer missing-file checks from folder listings while a server loads (0 = the game's own way)")
local fastFilesApplied, fastFilesActive
hook.Add("Think", "MeowyClientPack.FastFileChecks", function()
	local loading = IsInLoading() and not IsInGame()
	local want = fastFiles:GetBool()
	if want ~= fastFilesApplied and not IsInLoading() and not IsInGame() then
		fastFilesApplied = want
		local ok, why = meowy.FastFileChecks(want)
		if not ok then print("[Meowy Client Pack] fast file checks: " .. tostring(why)) end
	end
	local active = loading and fastFilesApplied == true
	if active ~= fastFilesActive then
		fastFilesActive = active
		meowy.FastFileChecksActive(active)
	end
end)
concommand.Add("meowy_fast_file_checks_status", function()
	print("[Meowy Client Pack] fast file checks: " .. meowy.FastFileChecksState())
end)

-- At the end of a server's Lua start-up the game throws every material away and
-- loads them all again (5-17 s on a big map). On our servers the server asks the
-- pack to skip that; this lets a player opt in for every server. Off by default:
-- a server whose downloads replace a material that's already loaded would show
-- the old one until the next map. Applied while a join loads, put back in game.
local matAnyServer = meowy.Setting("meowy_skipmatreload_anyserver", "0",
	"Meowy Client Pack: skip the second material load on every server, not just ours (a server's replaced textures can show their old version until the next map)")
local matPatchedHere
hook.Add("Think", "MeowyClientPack.MaterialReloadAnyServer", function()
	if IsInLoading() and not IsInGame() then
		if matPatchedHere == nil and matAnyServer:GetBool() then
			local ok, why = meowy.SkipMaterialReloadAnyServer()
			matPatchedHere = ok and true or false
			if not ok then print("[Meowy Client Pack] material reload not skipped: " .. tostring(why)) end
		end
	elseif matPatchedHere ~= nil then
		if matPatchedHere then meowy.RestoreMaterialReload() end
		matPatchedHere = nil
	end
end)

local stockGameDetails = GameDetails
if isfunction(stockGameDetails) then
	GameDetails = function(name, url, map, maxplayers, visible, steamid, ...)
		-- details[4]: the server shows a loading page of its own (as loading.lua decides).
		local page = isstring(url) and tonumber(maxplayers) and tonumber(maxplayers) > 1
			and (url:sub(1, 4) == "http" or url:sub(1, 8) == "asset://")
			and GetConVar("cl_enable_loadingurl") and GetConVar("cl_enable_loadingurl"):GetBool()
		details = { tostring(name or ""), tostring(map or ""), tostring(steamid or ""), page and true or false }
		-- The server's name, for the rejoin-after-a-crash card (resilience.lua).
		meowy.JoiningName = details[1]
		-- Our own SteamID64, so the server-list reports can leave our row out.
		if details[3]:match("^7656119%d+$") then meowy.SelfSteamID64 = details[3] end
		if active and sure then meowy.LoadingDetails(details[1], details[2], details[3]) end
		return stockGameDetails(name, url, map, maxplayers, visible, steamid, ...)
	end
end

hook.Add("Think", "MeowyClientPack.Loading", function()
	local loading = IsInLoading()
	if not active then
		if not loading then details, panelSince = nil, nil return end
		panelSince = panelSince or SysTime()
		if not enabled:GetBool() or not system.IsWindowed() then return end
		-- Start as soon as the engine says it's connecting to one of ours; the
		-- real connection confirms it a moment later (or ends it).
		local confirmed = meowy.LoadingTarget()
		if not confirmed and not meowy.LoadingHeadsUp() then
			-- Another server: once the connection says so and its details are in.
			local mode = everywhere:GetInt()
			if mode > 0 and details and (mode == 2 or not details[4]) and meowy.LoadingTargetState() == "other" then
				active, sure, lastStatus = true, true, GetLoadStatus()
				meowy.LoadingBeginOther((SysTime() - panelSince) * 1000, details[1], details[2])
			end
			return
		end
		-- Whatever status is showing now is left over from the last join (a retry
		-- starts on "Ready to play!"), so only changes from here on count.
		active, sure, lastStatus = true, confirmed, GetLoadStatus()
		meowy.LoadingBegin((SysTime() - panelSince) * 1000)
		if sure and details then meowy.LoadingDetails(details[1], details[2], details[3]) end
	end
	if not sure then
		local state = meowy.LoadingTargetState()
		if state == "ours" then
			sure = true
			if details then meowy.LoadingDetails(details[1], details[2], details[3]) end
		elseif state == "other" then
			active, details = false, nil
			meowy.LoadingEnd()
			return
		end
	end
	if meowy.LoadingCancelRequested() then RunConsoleCommand("disconnect") end
	if loading then
		local s = GetLoadStatus()
		if isstring(s) and s ~= lastStatus then
			lastStatus = s
			meowy.LoadingStatus(s)
		end
	else
		active, sure, details = false, false, nil
		if IsInGame() then meowy.LoadingInGame() else meowy.LoadingEnd() end
	end
end)
)lua";

#ifdef _WIN32
	// The copy the start-up stub loaded, when it isn't this one (pack_versions.h):
	// the stub loads an update from %LOCALAPPDATA% and says where in
	// MEOWY_PACK_MODULE, while a server's require("meowy") loads the installed
	// copy from lua\bin. That copy hands the client realm over to the stub's
	// (its exported `name`), so both realms share one copy. Only a module that's
	// already loaded is used; nothing is ever loaded from that variable.
	using ClientFn = int (*)(ILuaBase*);
	ClientFn HandOver(const char* name)
	{
		wchar_t path[MAX_PATH * 2];
		const DWORD n = GetEnvironmentVariableW(L"MEOWY_PACK_MODULE", path, MAX_PATH * 2);
		if (n == 0 || n >= MAX_PATH * 2)
			return nullptr;
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&HandOver), &self);
		HMODULE other = GetModuleHandleW(path);
		if (!other || other == self)
			return nullptr;
		return reinterpret_cast<ClientFn>(GetProcAddress(other, name));
	}
#endif
}

namespace
{
#ifdef _WIN32
// Discord status off, as the menu reads the setting: the saved meowy_discord
// (menu_settings.txt) or the policy. For when the menu's own Lua hasn't said:
// the client realm without the menu state, and the menu before its chunks run.
bool DiscordOffSaved()
{
	const policy::Policy p = policy::Effective();
	return discord::OffInMenuSettings(file_util::ReadSmall(DataDir() + L"\\menu_settings.txt", kMaxMenuSettings))
		|| std::find(p.off.begin(), p.off.end(), "meowy_discord") != p.off.end();
}
#endif

// A C++ exception out of a realm's open (std::bad_alloc in a 32-bit game, say)
// must never unwind into the game: the stack goes back to how the game passed
// it, one line says what happened, and the realm opens without the pack (or
// with what was set up before it). Lua's own errors aren't std::exceptions and
// pass through untouched, as in Guarded.
template <int (*Open)(ILuaBase*)>
int OpenGuarded(ILuaBase* LUA, const char* realm)
{
	const int top = LUA->Top();
	try
	{
		return Open(LUA);
	}
	catch (const std::exception& e)
	{
		const int pushed = LUA->Top() - top;
		if (pushed > 0)
			LUA->Pop(pushed);
		try
		{
			PrintLine(LUA, std::string("[Meowy Client Pack] the ") + realm + " realm couldn't open: " + e.what());
		}
		catch (const std::exception&)
		{
		}
	}
	return 0;
}

int ClientOpenBody(ILuaBase* LUA)
{
#ifdef _WIN32
	// Switched off by the signed policy (policy.h): only the version, nothing runs.
	// The menu realm's fetch already runs when the start-up stub loaded this
	// DLL (pinned); without it, one fetch on a thread holding its own reference
	// to the DLL, which the game frees after the realm closes.
	policy::StartFetching(PolicyDir(), startup_hook::MenuLoaded() ? policy::Fetch::Periodic : policy::Fetch::OnceUnpinned);
	if (!PolicyAllows())
	{
		LUA->PushSpecial(SPECIAL_GLOB);
		LUA->CreateTable();
		LUA->PushString(VERSION);
		LUA->SetField(-2, "VERSION");
		LUA->SetField(-2, "meowy");
		LUA->Pop();
		PrintLine(LUA, PolicyOffLine());
		return 0;
	}
#endif

	LUA->PushSpecial(SPECIAL_GLOB);
	LUA->CreateTable();

	LUA->PushString(VERSION);
	LUA->SetField(-2, "VERSION");
	SetFunction(LUA, "DiscordStart", Guarded<DiscordStart>);
	SetFunction(LUA, "DiscordSetActivity", Guarded<DiscordSetActivity>);
	SetFunction(LUA, "DiscordClear", Guarded<DiscordClear>);
	SetFunction(LUA, "DiscordState", Guarded<DiscordState>);
	SetFunction(LUA, "DiscordLastError", Guarded<DiscordLastError>);
	SetFunction(LUA, "CrashReports", Guarded<CrashReports>);
	SetFunction(LUA, "Hardware", Guarded<Hardware>);
	SetFunction(LUA, "SkipMaterialReload", Guarded<SkipMaterialReload>);
	SetFunction(LUA, "RestoreMaterialReload", Guarded<RestoreMaterialReload>);
	SetFunction(LUA, "MaterialReloadState", Guarded<MaterialReloadState>);
	SetFunction(LUA, "ServerAllowed", Guarded<ServerAllowed>);
	SetFunction(LUA, "LoadingDone", Guarded<LoadingDone>);
	SetFunction(LUA, "Notify", Guarded<Notify>);
	SetFunction(LUA, "SetNotifyEnabled", Guarded<SetNotifyEnabled>);
	SetFunction(LUA, "NotifyEnabled", Guarded<NotifyEnabled>);
	SetFunction(LUA, "NotifyCatalogue", Guarded<NotifyCatalogue>);
	SetFunction(LUA, "TimelineEvent", Guarded<TimelineEvent>);
	SetFunction(LUA, "ExpectRestart", Guarded<ExpectRestart>);

	LUA->SetField(-2, "meowy");
	LUA->Pop();

#ifdef _WIN32
	notify_toast::Configure(DataDir());
	// Without the menu state (the pack didn't start with the game), nothing has
	// said whether the player turned Discord status off: their saved setting and
	// the policy, read as the menu reads them, say.
	if (!startup_hook::MenuLoaded())
		discord::SetAllowed(!DiscordOffSaved());
#endif

	// Client Lua starting is the start of the join's longest stage, and the
	// menu can't report it: the game's main thread is busy until it's over.
	loading_screen::ClientLuaStarted();

#ifdef _WIN32
	// Loaded by our server's client Lua without having started with the game:
	// a Garry's Mod update (or a file check) put the stock menu.lua back. Put
	// the start-up line back for next time, unless the player turned it off.
	if (!startup_hook::MenuLoaded() && server_gate::Check() == server_gate::Result::Allowed)
	{
		const std::string did = startup_hook::Heal(GarrysModDir(), DataDir());
		if (did != "present" && did != "off")
		{
			LUA->PushSpecial(SPECIAL_GLOB);
			LUA->GetField(-1, "print");
			LUA->PushString(("[Meowy Client Pack] start-up line in lua/includes/menu.lua: " + did +
				(did == "added" ? " (a game update had removed it; the pack starts with the game again next time)" : "")).c_str());
			LUA->Call(1, 0);
			LUA->Pop();
		}
	}
	// Updates only start through the start-up stub (pack_versions.h), so without
	// "Start with the game" this copy stays as installed: say so once a start.
	// (The client realm doesn't load a downloaded copy itself: without the
	// stub's crash guard a broken update couldn't be rolled back.)
	static bool toldUpdates = false;
	if (!startup_hook::MenuLoaded() && !toldUpdates)
	{
		toldUpdates = true;
		pack_versions::Pointer pending;
		const bool waiting = pack_versions::ReadPointer(pack_versions::StatePath(DataDir(), L"pending"), &pending);
		LUA->PushSpecial(SPECIAL_GLOB);
		LUA->GetField(-1, "print");
		LUA->PushString((std::string("[Meowy Client Pack] ") + VERSION + " didn't start with the game, so it can't update itself"
			+ (waiting ? " (" + pending.version + " is downloaded and waiting)" : "")
			+ ". Turn on \"Start with the game\": run MeowyClientPack.exe again and leave it ticked.").c_str());
		LUA->Call(1, 0);
		LUA->Pop();
	}
#endif
	return 0;
}

int ClientOpen(ILuaBase* LUA)
{
	return OpenGuarded<ClientOpenBody>(LUA, "client");
}

int ClientClose(ILuaBase* LUA)
{
	(void)LUA;
	// The client realm closes as the player leaves the server (cancelled,
	// kicked or dropped before InitPostEntity put it back, too): the material
	// reload is never skipped for whatever is joined next. Unpinned (the pack
	// didn't start with the game), the DLL is freed after this, and with it the
	// only record of where the patch is. So no thread may run in an unpinned
	// copy without its own reference to the DLL (thread_util.h): the policy's
	// one-shot fetch and a toast worker hold one and keep the DLL mapped until
	// they end, and nothing here waits for them. Discord's worker is stopped
	// here with a bounded wait; one that doesn't stop in time holds its own
	// reference too.
	engine_patches::RestoreMaterialReload();
	discord::Shutdown();
	// No more notifications; the badge and the toasts' click handlers go.
	notify_toast::Shutdown();
	return 0;
}
}

// The client realm's entry points, also for an installed copy handing over (HandOver).
GMOD_DLL_EXPORT int meowy_client_open(ILuaBase* LUA) { return ClientOpen(LUA); }
GMOD_DLL_EXPORT int meowy_client_close(ILuaBase* LUA) { return ClientClose(LUA); }

GMOD_MODULE_OPEN()
{
#ifdef _WIN32
	if (ClientFn other = HandOver("meowy_client_open"))
		return other(LUA);
#endif
	return ClientOpen(LUA);
}

#ifdef _WIN32
namespace
{
// The menu realm's open (meowy_menu_open, below).
int MenuOpenBody(ILuaBase* LUA)
{
	// The kill switch first (policy.h): the policy kept from last time decides
	// this start; a newer one is fetched in the background and applies to
	// settings at once (meowy.Setting) and to the whole pack from the next start.
	policy::StartFetching(PolicyDir(), policy::Fetch::Periodic);
	const bool allowed = PolicyAllows();

	LUA->PushSpecial(SPECIAL_GLOB);
	LUA->CreateTable();
	LUA->PushString(VERSION);
	LUA->SetField(-2, "VERSION");
	LUA->PushString("menu");
	LUA->SetField(-2, "REALM");
	SetFunction(LUA, "PolicyState", Guarded<PolicyState>);
	SetFunction(LUA, "DiscordAllow", Guarded<DiscordAllow>);
	SetFunction(LUA, "UpdaterStart", Guarded<UpdaterStart>);
	SetFunction(LUA, "UpdaterState", Guarded<UpdaterState>);
	SetFunction(LUA, "UpdaterReady", Guarded<UpdaterReady>);
	if (!allowed)
	{
		// This version is revoked or too old, not the whole pack: a fixed one may
		// come, unless the player turned updates off (each check reads that too).
		if (policy::Effective().packEnabled && !pack_versions::UpdatesOff(DataDir()))
			updater::Start(DataDir(), VERSION, StubVersion());
		LUA->SetField(-2, "meowy");
		LUA->Pop();
		PrintLine(LUA, PolicyOffLine());
		return 0;
	}
	startup_hook::NoteMenuLoaded();
	// The saved Discord switch from the start, so it holds even if the menu
	// Lua that applies it (resilience.lua) didn't load.
	discord::SetAllowed(!DiscordOffSaved());
	SetFunction(LUA, "StartupHook", Guarded<StartupHookState>);
	SetFunction(LUA, "SetStartupHook", Guarded<SetStartupHook>);
	SetFunction(LUA, "LoadingTarget", Guarded<LoadingTarget>);
	SetFunction(LUA, "LoadingHeadsUp", Guarded<LoadingHeadsUp>);
	SetFunction(LUA, "LoadingTargetState", Guarded<LoadingTargetState>);
	SetFunction(LUA, "LoadingBegin", Guarded<LoadingBegin>);
	SetFunction(LUA, "LoadingBeginOther", Guarded<LoadingBeginOther>);
	SetFunction(LUA, "LoadingDetails", Guarded<LoadingDetails>);
	SetFunction(LUA, "LoadingStatus", Guarded<LoadingStatus>);
	SetFunction(LUA, "LoadingCancelRequested", Guarded<LoadingCancelRequested>);
	SetFunction(LUA, "LoadingInGame", Guarded<LoadingInGame>);
	SetFunction(LUA, "LoadingEnd", Guarded<LoadingEnd>);
	SetFunction(LUA, "LoadingPrewarm", Guarded<LoadingPrewarm>);
	SetFunction(LUA, "LoadingPark", Guarded<LoadingPark>);
	SetFunction(LUA, "ServerRepFetch", Guarded<ServerRepFetch>);
	SetFunction(LUA, "ServerRepTake", Guarded<ServerRepTake>);
	SetFunction(LUA, "ServerRepReport", Guarded<ServerRepReport>);
	SetFunction(LUA, "QueryPlayers", Guarded<QueryPlayers>);
	SetFunction(LUA, "TakePlayers", Guarded<TakePlayers>);
	SetFunction(LUA, "IsOurServer", Guarded<IsOurServer>);
	SetFunction(LUA, "OurServerAddress", Guarded<OurServerAddress>);
	SetFunction(LUA, "ServerNow", Guarded<ServerNow>);
	SetFunction(LUA, "MenuShellJs", Guarded<MenuShellJs>);
	SetFunction(LUA, "OtherServerAddress", Guarded<OtherServerAddress>);
	SetFunction(LUA, "StatusCaptureBegin", Guarded<StatusCaptureBegin>);
	SetFunction(LUA, "StatusCaptureEnd", Guarded<StatusCaptureEnd>);
	SetFunction(LUA, "ServerRepNote", Guarded<ServerRepNote>);
	SetFunction(LUA, "DataSandbox", Guarded<DataSandbox>);
	SetFunction(LUA, "DataSandboxDescribe", Guarded<DataSandboxDescribe>);
	SetFunction(LUA, "ResilienceWatch", Guarded<ResilienceWatch>);
	SetFunction(LUA, "ResilienceRestartExpected", Guarded<ResilienceRestartExpected>);
	SetFunction(LUA, "ResilienceLastDisconnect", Guarded<ResilienceLastDisconnect>);
	SetFunction(LUA, "ResilienceClearRestart", Guarded<ResilienceClearRestart>);
	SetFunction(LUA, "ResilienceProbe", Guarded<ResilienceProbe>);
	SetFunction(LUA, "ResilienceProbeResult", Guarded<ResilienceProbeResult>);
	SetFunction(LUA, "ResilienceSteamOnline", Guarded<ResilienceSteamOnline>);
	SetFunction(LUA, "ResilienceSettingsRead", Guarded<ResilienceSettingsRead>);
	SetFunction(LUA, "ResilienceSettingsWrite", Guarded<ResilienceSettingsWrite>);
	SetFunction(LUA, "MenuSettingsRead", Guarded<MenuSettingsRead>);
	SetFunction(LUA, "MenuSettingsWrite", Guarded<MenuSettingsWrite>);
	SetFunction(LUA, "MenuFileRead", Guarded<MenuFileRead>);
	SetFunction(LUA, "MenuFileWrite", Guarded<MenuFileWrite>);
	SetFunction(LUA, "ResilienceSession", Guarded<ResilienceSession>);
	SetFunction(LUA, "ResilienceCrashedSession", Guarded<ResilienceCrashedSession>);
	SetFunction(LUA, "PrewarmBeginJoin", Guarded<PrewarmBeginJoin>);
	SetFunction(LUA, "PrewarmEndJoin", Guarded<PrewarmEndJoin>);
	SetFunction(LUA, "PrewarmRecorded", Guarded<PrewarmRecorded>);
	SetFunction(LUA, "PrewarmStart", Guarded<PrewarmStart>);
	SetFunction(LUA, "PrewarmWorkshop", Guarded<PrewarmWorkshop>);
	SetFunction(LUA, "WorkshopItemState", Guarded<WorkshopItemState>);
	SetFunction(LUA, "WorkshopItemProgress", Guarded<WorkshopItemProgress>);
	SetFunction(LUA, "ContentAsk", Guarded<ContentAsk>);
	SetFunction(LUA, "ContentTake", Guarded<ContentTake>);
	SetFunction(LUA, "ContentState", Guarded<ContentState>);
	SetFunction(LUA, "PrewarmCancel", Guarded<PrewarmCancel>);
	SetFunction(LUA, "ConsoleCaptureStart", Guarded<ConsoleCaptureStart>);
	SetFunction(LUA, "ConsoleRead", Guarded<ConsoleRead>);
	SetFunction(LUA, "ConsoleState", Guarded<ConsoleState>);
	SetFunction(LUA, "ConsoleRun", Guarded<ConsoleRun>);
	SetFunction(LUA, "ConsoleFileRead", Guarded<ConsoleFileRead>);
	SetFunction(LUA, "ConsoleFileWrite", Guarded<ConsoleFileWrite>);
	SetFunction(LUA, "MaskPaths", Guarded<MaskPathsLua>);
	SetFunction(LUA, "ConsoleSaveLog", Guarded<ConsoleSaveLog>);
	SetFunction(LUA, "ServerQueryStart", Guarded<ServerQueryStart>);
	SetFunction(LUA, "ServerQueryTake", Guarded<ServerQueryTake>);
	SetFunction(LUA, "ServerQueryCancel", Guarded<ServerQueryCancel>);
	SetFunction(LUA, "ServerQueryState", Guarded<ServerQueryState>);
	SetFunction(LUA, "ServerIndexFetch", Guarded<ServerIndexFetch>);
	SetFunction(LUA, "ServerIndexTake", Guarded<ServerIndexTake>);
	SetFunction(LUA, "ServerIndexState", Guarded<ServerIndexState>);
	SetFunction(LUA, "PingMemoryLoad", Guarded<PingMemoryLoad>);
	SetFunction(LUA, "PingMemorySave", Guarded<PingMemorySave>);
	SetFunction(LUA, "PartyAvailable", Guarded<PartyAvailable>);
	SetFunction(LUA, "PartyStart", Guarded<PartyStart>);
	SetFunction(LUA, "PartyCreate", Guarded<PartyCreate>);
	SetFunction(LUA, "PartyJoin", Guarded<PartyJoin>);
	SetFunction(LUA, "PartyLeave", Guarded<PartyLeave>);
	SetFunction(LUA, "PartyInviteDialog", Guarded<PartyInviteDialog>);
	SetFunction(LUA, "PartyInvite", Guarded<PartyInvite>);
	SetFunction(LUA, "PartyInviteToServer", Guarded<PartyInviteToServer>);
	SetFunction(LUA, "PartySet", Guarded<PartySet>);
	SetFunction(LUA, "PartyState", Guarded<PartyState>);
	SetFunction(LUA, "PartyPoll", Guarded<PartyPoll>);
	SetFunction(LUA, "PartyFriends", Guarded<PartyFriends>);
	SetFunction(LUA, "PartyStatus", Guarded<PartyStatus>);
	SetFunction(LUA, "PrewarmState", Guarded<PrewarmState>);
	SetFunction(LUA, "PakCache", Guarded<PakCache>);
	SetFunction(LUA, "PakCacheState", Guarded<PakCacheState>);
	SetFunction(LUA, "ConfigBackup", Guarded<ConfigBackup>);
	SetFunction(LUA, "ConfigBackups", Guarded<ConfigBackups>);
	SetFunction(LUA, "ConfigRestore", Guarded<ConfigRestore>);
	SetFunction(LUA, "ConfigRestoreDone", Guarded<ConfigRestoreDone>);
	SetFunction(LUA, "JoinBoost", Guarded<JoinBoost>);
	SetFunction(LUA, "CvarHold", Guarded<CvarHold>);
	SetFunction(LUA, "CvarRelease", Guarded<CvarRelease>);
	SetFunction(LUA, "CvarHeld", Guarded<CvarHeld>);
	SetFunction(LUA, "JoinBoostState", Guarded<JoinBoostState>);
	SetFunction(LUA, "LastServerMark", Guarded<LastServerMark>);
	SetFunction(LUA, "LastServer", Guarded<LastServer>);
	SetFunction(LUA, "LastServerForget", Guarded<LastServerForget>);
	SetFunction(LUA, "DoctorFacts", Guarded<DoctorFacts>);
	SetFunction(LUA, "DoctorOpen", Guarded<DoctorOpen>);
	SetFunction(LUA, "AudioChanges", Guarded<AudioChanges>);
	SetFunction(LUA, "AudioState", Guarded<AudioState>);
	SetFunction(LUA, "FastLuaLookup", Guarded<FastLuaLookup>);
	SetFunction(LUA, "FastLuaLookupState", Guarded<FastLuaLookupState>);
	SetFunction(LUA, "FastLuaLookupNewJoin", Guarded<FastLuaLookupNewJoin>);
	SetFunction(LUA, "FastLuaFind", Guarded<FastLuaFind>);
	SetFunction(LUA, "FastLuaFindState", Guarded<FastLuaFindState>);
	SetFunction(LUA, "FastFonts", Guarded<FastFonts>);
	SetFunction(LUA, "FastFontsState", Guarded<FastFontsState>);
	SetFunction(LUA, "MouseConfine", Guarded<MouseConfine>);
	SetFunction(LUA, "MouseConfineState", Guarded<MouseConfineState>);
	SetFunction(LUA, "FastFileChecks", Guarded<FastFileChecks>);
	SetFunction(LUA, "FastFileChecksActive", Guarded<FastFileChecksActive>);
	SetFunction(LUA, "FastFileChecksState", Guarded<FastFileChecksState>);
	SetFunction(LUA, "SkipMaterialReloadAnyServer", Guarded<SkipMaterialReloadAnyServer>);
	SetFunction(LUA, "RestoreMaterialReload", Guarded<RestoreMaterialReload>);
	SetFunction(LUA, "MaterialReloadState", Guarded<MaterialReloadState>);
	SetFunction(LUA, "LauncherState", Guarded<LauncherState>);
	SetFunction(LUA, "LauncherTakeJoin", Guarded<LauncherTakeJoin>);
	LUA->SetField(-2, "meowy");

	// data/ was put right by the start-up stub (data_sandbox::Recover, under its
	// crash guard) before this was loaded.
	connect_watch::Start();
	launcher_pipe::Start(VERSION);

	const std::string resilienceLua = ResilienceChunk();
	// meowy.Setting first (settings.lua): every other chunk creates its settings with it.
	const std::string settingsLua = ResourceText(IDR_SETTINGS_LUA);
	for (const auto& chunk : { std::make_pair(settingsLua.c_str(), "meowy_client_pack/settings"),
		std::make_pair(kMenuLua, "meowy_client_pack/menu"),
		std::make_pair(kServerListLua, "meowy_client_pack/serverlist"),
		std::make_pair(resilienceLua.c_str(), "meowy_client_pack/resilience"),
		std::make_pair(launcher_pipe::kLua, "meowy_client_pack/launcher") })
	{
		if (!*chunk.first)
			continue;
		LUA->GetField(-1, "RunString");
		if (LUA->IsType(-1, Type::Function))
		{
			LUA->PushString(chunk.first);
			LUA->PushString(chunk.second);
			LUA->Call(2, 0);
		}
		else
		{
			LUA->Pop();
		}
	}

	// Settings → Apps shows the running version, also after an in-game update.
	apps_entry::RefreshVersion(apps_entry::kKey, VERSION);

	LUA->GetField(-1, "print");
	if (LUA->IsType(-1, Type::Function))
	{
		std::string msg = std::string("[Meowy Client Pack] ") + VERSION + " started with the game";
		LUA->PushString(msg.c_str());
		LUA->Call(1, 0);
	}
	else
	{
		LUA->Pop();
	}
	LUA->Pop();
	return 0;
}
}

// Menu-state entry point, called by gmsv_meowy_boot when the game starts
// (src/boot/boot.cpp). Only the version is exposed there for now; the menu
// realm is where the join features (our loading screen) will live.
GMOD_DLL_EXPORT int meowy_menu_open(ILuaBase* LUA)
{
	// The client realm's require("meowy") gets this same DLL and frees it when
	// the player leaves a server. Pinned, it stays loaded for the menu realm.
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		reinterpret_cast<LPCWSTR>(&meowy_menu_open), &self);
	return OpenGuarded<MenuOpenBody>(LUA, "menu");
}
#endif

GMOD_MODULE_CLOSE()
{
#ifdef _WIN32
	if (ClientFn other = HandOver("meowy_client_close"))
		return other(LUA);
#endif
	return ClientClose(LUA);
}
