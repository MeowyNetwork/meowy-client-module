// gmcl_meowy: optional client module for Meowy Network.
//
// Lua API (global table `meowy`, created by require("meowy")):
//   meowy.VERSION                     "1.3.0"
//   meowy.DiscordStart(clientId)      connect to the local Discord app
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
//   meowy.TimelineEvent(kind)         true | false, reason: a marker on Steam's
//                                     game-recording timeline ("Arrested"), whether
//                                     or not the game is in front. `kind` is one of
//                                     timeline.cpp's fixed list; title, description
//                                     and icon are the pack's. Reasons: "only works
//                                     on Meowy Network servers", "unknown kind",
//                                     "timeline unavailable", "rate limited". See
//                                     timeline.h, timeline_steam.h
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
// notifications' switch and the toast's logo), and WebView2's profile. The one change it can make to
// the game is SkipMaterialReload, in memory only, and only when Lua asks.

#include "GarrysMod/Lua/Interface.h"
#include "crash_reports.h"
#include "discord_ipc.h"
#include "engine_patches.h"
#include "hardware.h"
#include "connect_watch.h"
#include "loading_screen.h"
#include "server_gate.h"
#include "data_sandbox.h"
#include "server_rep.h"
#include "notify.h"
#include "notify_toast.h"
#include "timeline.h"
#include "timeline_steam.h"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace GarrysMod::Lua;

namespace
{
	const char* const VERSION = "1.5.0";

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
		std::vector<crashes::Report> reports = crashes::Since(since, max < 1 ? 1 : max > 20 ? 20 : static_cast<size_t>(max));

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

	int MaterialReloadState(lua_State* L)
	{
		L->luabase->PushString(engine_patches::MaterialReloadState());
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
#ifdef _WIN32
		ILuaBase* LUA = L->luabase;
		loading_screen::Begin(DataDir(), LUA->IsType(1, Type::Number) ? LUA->GetNumber(1) : -1);
		loading_screen::Note("watch", connect_watch::Describe());
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

	int LoadingPrewarm(lua_State*)
	{
#ifdef _WIN32
		loading_screen::Prewarm(DataDir());
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
	int ServerRepTake(lua_State* L)
	{
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

	// "ip:port" of the server the game is really connected to, when it's
	// someone else's (not ours, not singleplayer); else nil.
	// DataSandbox(onServer): data/ belongs to the server being joined or played
	// (onServer true), or to the menu (false). See data_sandbox.h.
	int DataSandbox(lua_State* L)
	{
		bool onServer = L->luabase->IsType(1, Type::Bool) && L->luabase->GetBool(1);
		if (!onServer)
		{
			data_sandbox::Leave();
			return 0;
		}
		// The real connection's address once there is one; until then the
		// address the engine printed as the join started.
		std::string address;
		server_gate::Result r = server_gate::Check(&address);
		if ((r != server_gate::Result::Allowed && r != server_gate::Result::OtherServer) || address.empty())
			address = connect_watch::LastAddress();
		if (address.empty())
			return 0;
		// Singleplayer, or hosting a game here: the player's own game, the menu's files.
		if (address.find("loopback") != std::string::npos || address.rfind("localhost", 0) == 0)
			data_sandbox::Leave();
		else
			data_sandbox::Enter(address);   // a P2P host or odd address gets a fixed sandbox of its own
		return 0;
	}

	int DataSandboxDescribe(lua_State* L)
	{
		L->luabase->PushString(data_sandbox::Describe().c_str());
		return 1;
	}

	int OtherServerAddress(lua_State* L)
	{
		std::string address;
		if (server_gate::Check(&address) == server_gate::Result::OtherServer && !address.empty()
			&& address.find("loopback") == std::string::npos && !server_gate::IsOurAddress(address))
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
local enabled = CreateConVar("meowy_honest_serverlist", "1", FCVAR_ARCHIVE,
	"Meowy Client Pack: check player counts in the server list (0 = off)")
local reports = CreateConVar("meowy_serverlist_reports", "1", FCVAR_ARCHIVE,
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
	for _, fn in ipairs(due) do fn() end
end)

-- Players' reports (who was really connected) beat everything; then this PC's
-- own fresh check; the crawler's verdict only for servers not checked here.
local function verdictOf(address)
	local r = rep[address]
	if r and r.source == "players" then return r end
	local m = mine[address]
	if m and SysTime() - m.at < RECHECK then return m.verdict ~= "ok" and m or nil end
	return r
end

local function tagFor(v)
	if v.verdict == "fake" then return "[FAKE - " .. tostring(v.real) .. " real] " end
	if v.verdict == "unverified" then
		-- A hidden list is a fact, not an accusation: big servers block the query against floods.
		local why = istable(v.reasons) and tostring(v.reasons[1] or "") or ""
		return why:find("hidden", 1, true) and "[LIST HIDDEN] " or "[UNVERIFIED] "
	end
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
if (!window.MeowyMark) window.MeowyMark = function (addr, real, tag) {
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
	} } }
	UpdateDigest(RootScope, 50);
};
]]

-- Shows a verdict that arrived after the server was already listed.
local function mark(address)
	local s = seen[address]
	if not s or not pnlMainMenu or tagged[s.type .. address] then return end
	local tag, v = tagOf(address)
	if not tag then return end
	tagged[s.type .. address] = true
	pnlMainMenu:Call(MARK_JS .. string.format("MeowyMark('%s', %d, '%s');",
		address:JavascriptSafe(), v and v.verdict == "fake" and (v.real or -1) or -1, tag:JavascriptSafe()))
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
				local times = other ~= address and o.joined[e.name]
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
			end
			return callback(ping, name, desc, map, players, maxplayers, botplayers, pass, lastplayed, address, ...)
		end
		data.Finished = function(...)
			if finished then finished(...) end
			startChecks(tostring(type))
		end
		fetchRep()
	end
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
				-- Our own row stays out: the report says nothing about who sent it.
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

local reported, joinedAt, nextLook = {}, {}, 0
hook.Add("Think", "MeowyClientPack.ServerReport", function()
	if SysTime() < nextLook then return end
	nextLook = SysTime() + 5
	if not enabled:GetBool() or not reports:GetBool() or not IsInGame() or IsInLoading() then return end
	local address = meowy.OtherServerAddress()
	if not address or reported[address] then return end
	joinedAt[address] = joinedAt[address] or SysTime()
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
local enabled = CreateConVar("meowy_loadingscreen", "1", FCVAR_ARCHIVE,
	"Meowy Client Pack: our own loading screen when joining Meowy Network (0 = Garry's Mod's)")
local active, sure, lastStatus, details = false, false, nil, nil

-- Load the screen's page now, hidden, so a join shows it at once.
if enabled:GetBool() and system.IsWindowed() then meowy.LoadingPrewarm() end
local panelSince -- SysTime when GMod's loading panel opened, for how long it showed before ours

-- A separate garrysmod/data for each server (data_sandbox.h): a server's Lua
-- sees only the files it wrote itself, never other servers' or the menu's.
local sandbox = CreateConVar("meowy_data_sandbox", "1", FCVAR_ARCHIVE,
	"Meowy Client Pack: give every server its own garrysmod/data (0 = one data folder for all)")
hook.Add("Think", "MeowyClientPack.DataSandbox", function()
	meowy.DataSandbox(sandbox:GetBool() and (IsInLoading() or IsInGame()))
end)

local stockGameDetails = GameDetails
if isfunction(stockGameDetails) then
	GameDetails = function(name, url, map, maxplayers, visible, steamid, ...)
		details = { tostring(name or ""), tostring(map or ""), tostring(steamid or "") }
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
		if not confirmed and not meowy.LoadingHeadsUp() then return end
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
}

GMOD_MODULE_OPEN()
{
	LUA->PushSpecial(SPECIAL_GLOB);
	LUA->CreateTable();

	LUA->PushString(VERSION);
	LUA->SetField(-2, "VERSION");
	SetFunction(LUA, "DiscordStart", DiscordStart);
	SetFunction(LUA, "DiscordSetActivity", DiscordSetActivity);
	SetFunction(LUA, "DiscordClear", DiscordClear);
	SetFunction(LUA, "DiscordState", DiscordState);
	SetFunction(LUA, "DiscordLastError", DiscordLastError);
	SetFunction(LUA, "CrashReports", CrashReports);
	SetFunction(LUA, "Hardware", Hardware);
	SetFunction(LUA, "SkipMaterialReload", SkipMaterialReload);
	SetFunction(LUA, "RestoreMaterialReload", RestoreMaterialReload);
	SetFunction(LUA, "MaterialReloadState", MaterialReloadState);
	SetFunction(LUA, "ServerAllowed", ServerAllowed);
	SetFunction(LUA, "LoadingDone", LoadingDone);
	SetFunction(LUA, "Notify", Notify);
	SetFunction(LUA, "SetNotifyEnabled", SetNotifyEnabled);
	SetFunction(LUA, "NotifyEnabled", NotifyEnabled);
	SetFunction(LUA, "TimelineEvent", TimelineEvent);

	LUA->SetField(-2, "meowy");
	LUA->Pop();

#ifdef _WIN32
	notify_toast::Configure(DataDir());
#endif

	// Client Lua starting is the start of the join's longest stage, and the
	// menu can't report it: the game's main thread is busy until it's over.
	loading_screen::ClientLuaStarted();
	return 0;
}

#ifdef _WIN32
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

	LUA->PushSpecial(SPECIAL_GLOB);
	LUA->CreateTable();
	LUA->PushString(VERSION);
	LUA->SetField(-2, "VERSION");
	LUA->PushString("menu");
	LUA->SetField(-2, "REALM");
	SetFunction(LUA, "LoadingTarget", LoadingTarget);
	SetFunction(LUA, "LoadingHeadsUp", LoadingHeadsUp);
	SetFunction(LUA, "LoadingTargetState", LoadingTargetState);
	SetFunction(LUA, "LoadingBegin", LoadingBegin);
	SetFunction(LUA, "LoadingDetails", LoadingDetails);
	SetFunction(LUA, "LoadingStatus", LoadingStatus);
	SetFunction(LUA, "LoadingCancelRequested", LoadingCancelRequested);
	SetFunction(LUA, "LoadingInGame", LoadingInGame);
	SetFunction(LUA, "LoadingEnd", LoadingEnd);
	SetFunction(LUA, "LoadingPrewarm", LoadingPrewarm);
	SetFunction(LUA, "ServerRepFetch", ServerRepFetch);
	SetFunction(LUA, "ServerRepTake", ServerRepTake);
	SetFunction(LUA, "ServerRepReport", ServerRepReport);
	SetFunction(LUA, "QueryPlayers", QueryPlayers);
	SetFunction(LUA, "TakePlayers", TakePlayers);
	SetFunction(LUA, "IsOurServer", IsOurServer);
	SetFunction(LUA, "OtherServerAddress", OtherServerAddress);
	SetFunction(LUA, "StatusCaptureBegin", StatusCaptureBegin);
	SetFunction(LUA, "StatusCaptureEnd", StatusCaptureEnd);
	SetFunction(LUA, "ServerRepNote", ServerRepNote);
	SetFunction(LUA, "DataSandbox", DataSandbox);
	SetFunction(LUA, "DataSandboxDescribe", DataSandboxDescribe);
	LUA->SetField(-2, "meowy");

	data_sandbox::Recover();
	connect_watch::Start();

	for (const auto& chunk : { std::make_pair(kMenuLua, "meowy_client_pack/menu"),
		std::make_pair(kServerListLua, "meowy_client_pack/serverlist") })
	{
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
#endif

GMOD_MODULE_CLOSE()
{
	(void)LUA;
	discord::Shutdown();
	// The client realm closes as the player leaves the server: no more
	// notifications, and the badge and the toasts' click handlers go.
	notify_toast::Shutdown();
	return 0;
}
