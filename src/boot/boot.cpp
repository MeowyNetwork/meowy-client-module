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
// launch options, and nothing is loaded. Nor when Meowy Network's kill switch
// says so (state\policy.bin, from the pack's signed policy). If the game died while the module
// was loading last time (%LOCALAPPDATA%\MeowyClientPack\boot.lock was left behind), this launch
// skips it once.
//
// The per-server data/ (data_sandbox.h) is put right by the stub itself, before
// any of that is decided: after a crash on a server, data/ still links to that
// server's folder, and a start without the pack (safe mode, the skip after a
// crash) would otherwise show it to every server joined that session and file
// what they write under that first server. It's file-system calls only, nothing
// in the game. The pack itself never does this again: only the stub runs it,
// under its own crash guard.

#include "GarrysMod/Lua/Interface.h"
#include "../data_sandbox.h"
#include "../file_util.h"
#include "../pack_versions.h"

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

	// data_sandbox::Recover(), guarded by a lock of its own: if the game went
	// down during this very recovery last time, it's skipped this once (safe
	// mode must always get the game to start).
	void RecoverDataSandbox(ILuaBase* LUA, const std::wstring& dataDir)
	{
		if (dataDir.empty())
			return;
		const std::wstring lock = dataDir + L"\\recover.lock";
		if (GetFileAttributesW(lock.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			DeleteFileW(lock.c_str());
			Print(LUA, "[Meowy Client Pack] the game closed while garrysmod/data was being put back last time; "
				"skipping that this once");
			return;
		}
		HANDLE h = CreateFileW(lock.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE)
			CloseHandle(h);
		const std::string said = data_sandbox::Recover();
		DeleteFileW(lock.c_str());
		if (!said.empty())
			Print(LUA, ("[Meowy Client Pack] " + said).c_str());
	}

	// Meowy Network's kill switch (the pack's policy.h): state\policy.bin, written
	// by the pack from a signed policy, says "enabled 0" to keep the pack off.
	// Read here with no crypto and no network; it can only switch the pack off.
	// Returns the line to print when it's off, else "".
	std::string PolicyOff(const std::wstring& dataDir)
	{
		if (dataDir.empty())
			return std::string();
		HANDLE h = CreateFileW((dataDir + L"\\state\\policy.bin").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return std::string();
		char buf[512] = {};
		DWORD read = 0;
		ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
		CloseHandle(h);
		const std::string text(buf, read);
		if (text.compare(0, 10, "enabled 0\n") != 0)
			return std::string();
		std::string line = "[Meowy Client Pack] switched off by Meowy Network; not loaded at start-up";
		const size_t notice = text.find("\nnotice ");
		if (notice != std::string::npos)
		{
			const size_t start = notice + 8, end = text.find('\n', start);
			line += ": " + text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		}
		return line;
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
	const std::wstring dataDir = PackDataDir();
	RecoverDataSandbox(LUA, dataDir);

	if (SafeModeRequested())
	{
		Print(LUA, "[Meowy Client Pack] safe mode: not loaded at start-up");
		return 0;
	}

	const std::string off = PolicyOff(dataDir);
	if (!off.empty())
	{
		Print(LUA, off.c_str());
		return 0;
	}

	const std::wstring bin = OwnFolder();
	if (bin.empty())
		return 0;
	// Before 1.4.1 the lock was in garrysmod\data\meowy.
	DeleteFileW((bin + L"..\\..\\data\\meowy\\boot.lock").c_str());

	const std::wstring lock = dataDir.empty() ? L"" : dataDir + L"\\boot.lock";

	// An update downloaded last session (pack_versions.h) is taken now, unless
	// the player turned updates off since (it waits until they're back on).
	if (!dataDir.empty() && !pack_versions::UpdatesOff(dataDir))
	{
		const std::string promoted = pack_versions::Promote(dataDir);
		if (!promoted.empty())
			Print(LUA, ("[Meowy Client Pack] " + promoted).c_str());
	}

	if (!lock.empty() && GetFileAttributesW(lock.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		// The lock holds the version that was starting. A fresh update that took
		// the game down goes back to the version before it; anything else is
		// skipped this once, as before updates existed.
		std::string version = file_util::ReadSmall(lock, 64);
		while (!version.empty() && (version.back() == '\n' || version.back() == '\r'))
			version.pop_back();
		DeleteFileW(lock.c_str());
		std::string said;
		if (pack_versions::RevertAfterCrash(dataDir, version, said))
		{
			Print(LUA, ("[Meowy Client Pack] " + said).c_str());
		}
		else
		{
			Print(LUA, "[Meowy Client Pack] the game closed while the pack was starting last time; "
				"skipping it at start-up this once (it still loads when you join Meowy Network)");
			return 0;
		}
	}

	// The newest copy that verifies and the policy allows, else the installed one.
	pack_versions::Choice choice = dataDir.empty() ? pack_versions::Choice() : pack_versions::Choose(dataDir);
	if (!choice.why.empty())
		Print(LUA, ("[Meowy Client Pack] " + choice.why).c_str());
	const std::wstring installed = bin + pack_versions::DllName();
	std::wstring module = choice.path.empty() ? installed : choice.path;

	if (!lock.empty())
		file_util::WriteAtomic(lock, choice.version + "\n");

	HMODULE pack = LoadLibraryExW(module.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!pack && module != installed)
	{
		// An update that won't even load: never again, and the installed copy now.
		pack_versions::MarkBad(dataDir, choice.version);
		Print(LUA, ("[Meowy Client Pack] " + choice.version + " wouldn't load; using the installed copy").c_str());
		choice = pack_versions::Choice();
		module = installed;
		if (!lock.empty())
			file_util::WriteAtomic(lock, choice.version + "\n");
		pack = LoadLibraryExW(module.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	}
	if (!pack)
	{
		DeleteFileW(lock.c_str());
		Print(LUA, "[Meowy Client Pack] couldn't load the pack at start-up (is it installed?)");
		return 0;
	}
	// The client realm's require("meowy") loads the installed copy from lua\bin;
	// it hands over to this one (module.cpp), so both realms share one copy.
	SetEnvironmentVariableW(L"MEOWY_PACK_MODULE", module.c_str());

	auto open = reinterpret_cast<MenuOpen>(GetProcAddress(pack, "meowy_menu_open"));
	if (open)
		open(LUA);
	else
		Print(LUA, "[Meowy Client Pack] the installed pack is too old to start with the game");

	DeleteFileW(lock.c_str());
	if (!dataDir.empty() && choice.version != "installed")
		pack_versions::NoteLoaded(dataDir, choice.version);
	return 0;
}

// The pack's updater only stages versions a stub can load: this one is 1 (it
// loads %LOCALAPPDATA%\MeowyClientPack\bin\<version>, pack_versions.h).
extern "C" __declspec(dllexport) int meowy_stub_version()
{
	return 1;
}

GMOD_MODULE_CLOSE()
{
	(void)LUA;
	return 0;
}
