// gmsv_meowy_boot: loads the Client Pack in GMod's menu state, at game start.
//
// The installer adds one line to garrysmod/lua/includes/menu.lua:
//   pcall(require, "meowy_boot") -- meowy-client-pack ...
// In the menu state require() looks for gmsv_ modules (IsClient() is false
// there), so this small stub is what loads. It loads the one real module,
// gmcl_meowy_<arch>.dll, from the same folder and calls its menu entry point.
// When a server's client Lua later does require("meowy"), Windows hands back
// that same loaded DLL, so both realms share one copy (one patch state).
//
// Safe mode: hold Shift while the game starts, or add -meowy_safe to the
// launch options, and nothing is loaded. If the game died while the module
// was loading last time (%LOCALAPPDATA%\MeowyClientPack\boot.lock was left behind), this launch
// skips it once.

#include "GarrysMod/Lua/Interface.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwchar>
#include <string>

using namespace GarrysMod::Lua;

namespace
{
	using MenuOpen = int (*)(ILuaBase*);

	void Print(ILuaBase* LUA, const char* msg)
	{
		LUA->PushSpecial(SPECIAL_GLOB);
		LUA->GetField(-1, "print");
		if (LUA->IsType(-1, Type::Function))
		{
			LUA->PushString(msg);
			LUA->Call(1, 0);
		}
		else
		{
			LUA->Pop();
		}
		LUA->Pop();
	}

	// The folder this stub was loaded from (garrysmod\lua\bin), with a trailing backslash.
	std::wstring OwnFolder()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&OwnFolder), &self);
		wchar_t path[MAX_PATH];
		DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return L"";
		std::wstring s(path, n);
		size_t slash = s.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
	}

	// %LOCALAPPDATA%\MeowyClientPack, created if missing; empty if there's no LOCALAPPDATA.
	// Not garrysmod\data, which every server's Lua can read: a file there would tell
	// any server that this player has the pack.
	std::wstring PackDataDir()
	{
		wchar_t base[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return L"";
		std::wstring dir = std::wstring(base) + L"\\MeowyClientPack";
		CreateDirectoryW(dir.c_str(), nullptr);
		return dir;
	}

	bool SafeModeRequested()
	{
		if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
			return true;
		return std::wcsstr(GetCommandLineW(), L"-meowy_safe") != nullptr;
	}
}

GMOD_MODULE_OPEN()
{
	if (SafeModeRequested())
	{
		Print(LUA, "[Meowy Client Pack] safe mode: not loaded at start-up");
		return 0;
	}

	const std::wstring bin = OwnFolder();
	if (bin.empty())
		return 0;
	// Before 1.4.1 the lock was in garrysmod\data\meowy.
	DeleteFileW((bin + L"..\\..\\data\\meowy\\boot.lock").c_str());

	const std::wstring dataDir = PackDataDir();
	const std::wstring lock = dataDir.empty() ? L"" : dataDir + L"\\boot.lock";

	if (!lock.empty() && GetFileAttributesW(lock.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		DeleteFileW(lock.c_str());
		Print(LUA, "[Meowy Client Pack] the game closed while the pack was starting last time; "
			"skipping it at start-up this once (it still loads when you join Meowy Network)");
		return 0;
	}

	if (!lock.empty())
	{
		HANDLE h = CreateFileW(lock.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE)
			CloseHandle(h);
	}

#ifdef _WIN64
	const std::wstring module = bin + L"gmcl_meowy_win64.dll";
#else
	const std::wstring module = bin + L"gmcl_meowy_win32.dll";
#endif
	HMODULE pack = LoadLibraryExW(module.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!pack)
	{
		DeleteFileW(lock.c_str());
		Print(LUA, "[Meowy Client Pack] couldn't load the pack at start-up (is it installed?)");
		return 0;
	}

	auto open = reinterpret_cast<MenuOpen>(GetProcAddress(pack, "meowy_menu_open"));
	if (open)
		open(LUA);
	else
		Print(LUA, "[Meowy Client Pack] the installed pack is too old to start with the game");

	DeleteFileW(lock.c_str());
	return 0;
}

GMOD_MODULE_CLOSE()
{
	(void)LUA;
	return 0;
}
