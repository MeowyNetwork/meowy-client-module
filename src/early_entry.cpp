// PROTOTYPE (launcher/DESIGN.md §9, start-up #9): the pack's start-up savers
// from the game's first instruction. Not used by the game, the stub or the
// launcher; only launcher/tools/early_entry (a measurement harness) calls it,
// in a gmod.exe it started suspended and loaded this DLL into.
//
// meowy_early_start, run on a thread of its own before the game's main thread
// runs, starts a watcher that:
//   - installs fs_cache (missing-file answers for FS_fopen) as soon as
//     filesystem_stdio's object is the stock one, and turns it on;
//   - installs font_loads (no LoadLibrary/FreeLibrary of gdi32 per font) as
//     soon as vguimatsurface.dll is loaded;
//   - turns fs_cache off again when the pack's menu entry runs (start-up done),
//     so joins use it exactly as before;
//   - writes what it did, with times, to %LOCALAPPDATA%\MeowyClientPack\early.txt.
// Each install refuses on anything unexpected (fs_cache and font_loads check
// the code they touch), and then the game simply starts as it would.

#include "fs_cache.h"
#include "font_loads.h"
#include "startup_hook.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace
{
	std::string g_log;
	ULONGLONG g_t0 = 0;

	void Note(const std::string& what)
	{
		g_log += std::to_string(GetTickCount64() - g_t0) + " ms: " + what + "\r\n";
	}

	void Save()
	{
		wchar_t base[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return;
		const std::wstring file = std::wstring(base) + L"\\MeowyClientPack\\early.txt";
		HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		DWORD w = 0;
		WriteFile(h, g_log.data(), static_cast<DWORD>(g_log.size()), &w, nullptr);
		CloseHandle(h);
	}

	DWORD WINAPI Watch(LPVOID)
	{
		bool fsDone = false, fontsDone = false;
		std::string fsWhy, fontsWhy;
		// 1 ms steps until both are in (or 60 s), then wait for the menu.
		for (int i = 0; i < 60000 && !(fsDone && fontsDone); ++i)
		{
			if (!fsDone && GetModuleHandleW(L"filesystem_stdio.dll"))
			{
				std::string why;
				if (fs_cache::Install(why))
				{
					fs_cache::SetActive(true);
					fsDone = true;
					Note("fs_cache installed and on");
				}
				else
					fsWhy = why;   // the object may not be built yet: try again
			}
			if (!fontsDone && GetModuleHandleW(L"vguimatsurface.dll"))
			{
				std::string why;
				if (font_loads::Install(why))
				{
					fontsDone = true;
					Note("font_loads installed");
				}
				else
					fontsWhy = why;
			}
			if (startup_hook::MenuLoaded())
				break;
			Sleep(1);
		}
		if (!fsDone)
			Note("fs_cache not installed: " + fsWhy);
		if (!fontsDone)
			Note("font_loads not installed: " + fontsWhy);
		for (int i = 0; i < 120000 && !startup_hook::MenuLoaded(); ++i)
			Sleep(1);
		fs_cache::SetActive(false);
		Note("menu reached; fs_cache off. " + fs_cache::Describe());
		Note(font_loads::Describe());
		Save();
		return 0;
	}
}

extern "C" __declspec(dllexport) DWORD WINAPI meowy_early_start(LPVOID)
{
	g_t0 = GetTickCount64();
	Note("early start");
	HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
	if (t)
		CloseHandle(t);
	return 0;
}
#endif
