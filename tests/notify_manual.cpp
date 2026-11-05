// notify_manual: shows one real notification on this PC, the way the game
// would, for checking by eye (see tests/NOTIFY_MANUAL.md).
//   notify_manual [kind] [seconds] [--force-toast]
//   notify_manual --check-shortcut <path.lnk>
// --force-toast tries the WinRT toast even without the Start-menu shortcut
// (checks that code end to end; without the shortcut Windows may drop it).
// Opens a small window standing in for the game's, minimises it (so the
// "game" isn't in front), queues `kind` (default printer_attacked) through the
// same worker the module uses, and keeps running `seconds` (default 20) so a
// click on the toast or balloon can bring the window back. Prints which path
// was taken ("toast", or the balloon fallback and why) and how long the
// worker took to shut down.
#include "../src/notify.h"
#include "../src/notify_toast.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

static LRESULT CALLBACK StandInProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static std::string Narrow(const wchar_t* s)
{
	std::string out;
	for (; *s; ++s)
		out += *s < 128 ? static_cast<char>(*s) : '?';
	return out;
}

static void Pump(DWORD ms)
{
	ULONGLONG until = GetTickCount64() + ms;
	while (GetTickCount64() < until)
	{
		MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
}

int wmain(int argc, wchar_t** argv)
{
	// notify_manual --check-shortcut <path.lnk>: does the module accept this
	// shortcut (e.g. one the installer made) as carrying our AUMID? Exit 0 if so.
	if (argc == 3 && std::wcscmp(argv[1], L"--check-shortcut") == 0)
	{
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		bool ok = notify_toast::ShortcutHasAppId(argv[2]);
		std::printf("shortcut carries MeowyNetwork.ClientPack: %s\n", ok ? "yes" : "no");
		CoUninitialize();
		return ok ? 0 : 1;
	}

	std::string kindArg = argc > 1 ? Narrow(argv[1]) : std::string();
	bool forceToast = argc > 1 && std::wcscmp(argv[argc - 1], L"--force-toast") == 0;
	if (forceToast)
		--argc;
	const char* kind = argc > 1 ? kindArg.c_str() : "printer_attacked";
	int seconds = argc > 2 ? _wtoi(argv[2]) : 20;
	notify_toast::ForceToastPathForTest(forceToast);
	int index = notify::FindKind(kind, std::strlen(kind));
	if (index < 0)
	{
		std::printf("unknown kind: %s\n", kind);
		return 2;
	}

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc = StandInProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"MeowyNotifyManualStandIn";
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	RegisterClassExW(&wc);
	HWND window = CreateWindowExW(0, wc.lpszClassName, L"notify_manual (stands in for Garry's Mod)",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 480, 200, nullptr, nullptr, wc.hInstance, nullptr);
	ShowWindow(window, SW_SHOWMINNOACTIVE);
	Pump(500);

	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	std::wstring dir = std::wstring(temp) + L"meowy_notify_manual";
	CreateDirectoryW(dir.c_str(), nullptr);
	notify_toast::Configure(dir);
	notify_toast::SetTargetWindowForTest(window);

	std::printf("game focused before posting: %s\n", notify_toast::GameFocused() ? "yes (it'll be skipped)" : "no");
	bool posted = notify_toast::Post(index);
	std::printf("posted %s: %s\n", kind, posted ? "yes" : "no");
	Pump(2000);
	std::printf("result: %s\n", notify_toast::LastResult().c_str());
	std::printf("waiting %d s: click the notification to bring the stand-in window forward...\n", seconds);
	for (int i = 0; i < seconds; ++i)
	{
		Pump(1000);
		std::printf("  %2d s: stand-in in front: %s, minimised: %s, %s\n", i + 1, GetForegroundWindow() == window ? "yes" : "no",
			IsIconic(window) ? "yes" : "no", notify_toast::LastResult().c_str());
	}

	ULONGLONG before = GetTickCount64();
	notify_toast::Shutdown();
	std::printf("shutdown took %llu ms\n", GetTickCount64() - before);
	DestroyWindow(window);
	return 0;
}
