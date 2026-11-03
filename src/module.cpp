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
//   meowy.SkipMaterialReload()        true | false, reason (1.3+, 64-bit only): stop
//                                     GMod reloading every material at the end of
//                                     client Lua start-up; call from client autorun
//   meowy.RestoreMaterialReload()     undo it
//   meowy.MaterialReloadState()       "patched" | "not patched" | "unsupported"
//
// DiscordSetActivity fields, all optional:
//   details, state       strings, shown on two lines
//   start                unix time; Discord shows "elapsed"
//   large_image, large_text
//   party_size, party_max
//   buttons              up to 2 { label = "...", url = "https://..." }
//
// The module only talks to Discord's local IPC pipe, reads the game's own
// crashes/ folder and asks Windows for the CPU/GPU/RAM description. It makes
// no network connections and writes no files. The one change it can make to
// the game is SkipMaterialReload, in memory only, and only when Lua asks.

#include "GarrysMod/Lua/Interface.h"
#include "crash_reports.h"
#include "discord_ipc.h"
#include "engine_patches.h"
#include "hardware.h"

#include <cstdio>
#include <string>
#include <vector>

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

	bool IsHttpUrl(const std::string& url)
	{
		return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
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

	int DiscordStart(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
		const char* id = LUA->CheckString(1);
		if (!IsSnowflake(id))
			LUA->ArgError(1, "not a Discord application id");
		discord::Start(id);
		return 0;
	}

	int DiscordSetActivity(lua_State* L)
	{
		ILuaBase* LUA = L->luabase;
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

	LUA->SetField(-2, "meowy");
	LUA->Pop();
	return 0;
}

GMOD_MODULE_CLOSE()
{
	(void)LUA;
	discord::Shutdown();
	return 0;
}
