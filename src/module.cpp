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
// The module only talks to Discord's local IPC pipe, reads the game's own
// crashes/ folder and asks Windows for the CPU/GPU/RAM description. It makes
// no network connections. It writes only in garrysmod/data/meowy: the
// start-up stub's boot.lock, and the loading screen's join_times.txt and
// last_join.txt (loading_screen.h). The one change it can make to
// the game is SkipMaterialReload, in memory only, and only when Lua asks.

#include "GarrysMod/Lua/Interface.h"
#include "crash_reports.h"
#include "discord_ipc.h"
#include "engine_patches.h"
#include "hardware.h"
#include "loading_screen.h"
#include "server_gate.h"

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
	const char* const VERSION = "1.3.0";

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
	// garrysmod\data\meowy, from this DLL's own path (garrysmod\lua\bin).
	std::wstring DataDir()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&DataDir), &self);
		wchar_t path[MAX_PATH];
		DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
		std::wstring s(path, n < MAX_PATH ? n : 0);
		size_t slash = s.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1) + L"..\\..\\data\\meowy";
	}
#endif

	// Menu: is the join that's loading one to our server? Same check as everything else.
	int LoadingTarget(lua_State* L)
	{
		L->luabase->PushBool(server_gate::Check() == server_gate::Result::Allowed);
		return 1;
	}

	int LoadingBegin(lua_State*)
	{
		if (server_gate::Check() != server_gate::Result::Allowed)
			return 0;
#ifdef _WIN32
		loading_screen::Begin(DataDir());
#endif
		return 0;
	}

	int LoadingDetails(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		loading_screen::Details(LUA->CheckString(1), LUA->CheckString(2),
			LUA->IsType(3, Type::String) ? LUA->GetString(3) : "");
		return 0;
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
		loading_screen::InGame();
		return 0;
	}

	int LoadingEnd(lua_State*)
	{
		loading_screen::End();
		return 0;
	}

	// Client: the player has spawned and the game runs smoothly; fade out.
	int LoadingDone(lua_State*)
	{
		loading_screen::Done();
		return 0;
	}

	// Runs in the menu state. The loading panel is active from the moment a
	// connect starts (IsInLoading); once the engine's connection is to one of
	// our servers, our screen takes over. It only gets fed while the game's
	// main thread runs, but draws on its own thread regardless.
	const char* const kMenuLua = R"lua(
local meowy = meowy
local enabled = CreateConVar("meowy_loadingscreen", "1", FCVAR_ARCHIVE,
	"Meowy Client Pack: our own loading screen when joining Meowy Network (0 = Garry's Mod's)")
local active, lastStatus, details = false, nil, nil

local stockGameDetails = GameDetails
if isfunction(stockGameDetails) then
	GameDetails = function(name, url, map, maxplayers, visible, steamid, ...)
		details = { tostring(name or ""), tostring(map or ""), tostring(steamid or "") }
		if active then meowy.LoadingDetails(details[1], details[2], details[3]) end
		return stockGameDetails(name, url, map, maxplayers, visible, steamid, ...)
	end
end

hook.Add("Think", "MeowyClientPack.Loading", function()
	local loading = IsInLoading()
	if not active then
		if not loading then details = nil return end
		if not enabled:GetBool() or not system.IsWindowed() or not meowy.LoadingTarget() then return end
		active, lastStatus = true, nil
		meowy.LoadingBegin()
		if details then meowy.LoadingDetails(details[1], details[2], details[3]) end
	end
	if meowy.LoadingCancelRequested() then RunConsoleCommand("disconnect") end
	if loading then
		local s = GetLoadStatus()
		if isstring(s) and s ~= lastStatus then
			lastStatus = s
			meowy.LoadingStatus(s)
		end
	else
		active, details = false, nil
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

	LUA->SetField(-2, "meowy");
	LUA->Pop();

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
	SetFunction(LUA, "LoadingBegin", LoadingBegin);
	SetFunction(LUA, "LoadingDetails", LoadingDetails);
	SetFunction(LUA, "LoadingStatus", LoadingStatus);
	SetFunction(LUA, "LoadingCancelRequested", LoadingCancelRequested);
	SetFunction(LUA, "LoadingInGame", LoadingInGame);
	SetFunction(LUA, "LoadingEnd", LoadingEnd);
	LUA->SetField(-2, "meowy");

	LUA->GetField(-1, "RunString");
	if (LUA->IsType(-1, Type::Function))
	{
		LUA->PushString(kMenuLua);
		LUA->PushString("meowy_client_pack/menu");
		LUA->Call(2, 0);
	}
	else
	{
		LUA->Pop();
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
	return 0;
}
