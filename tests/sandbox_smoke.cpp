// sandbox_smoke: runs data_sandbox against a throwaway GarrysMod folder.
//   sandbox_smoke <empty scratch folder>
// Builds <scratch>\GarrysMod\garrysmod\data with a few files, points LOCALAPPDATA
// at <scratch>\local, and walks: menu -> server A -> server B -> crash -> recover.
#include "../src/data_sandbox.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>

static int g_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		++g_failed;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static void Touch(const std::wstring& p)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2)
	{
		std::printf("usage: sandbox_smoke <empty scratch folder>\n");
		return 2;
	}
	const std::wstring root = argv[1];
	const std::wstring game = root + L"\\GarrysMod", data = game + L"\\garrysmod\\data", local = root + L"\\local";
	CreateDirectoryW(root.c_str(), nullptr);
	CreateDirectoryW(game.c_str(), nullptr);
	CreateDirectoryW((game + L"\\garrysmod").c_str(), nullptr);
	CreateDirectoryW(data.c_str(), nullptr);
	CreateDirectoryW(local.c_str(), nullptr);
	CreateDirectoryW((data + L"\\zrms").c_str(), nullptr);
	Touch(data + L"\\zrms\\settings.txt");
	Touch(data + L"\\193_243_190_11_27041_nexus_inventory_slots.txt");
	SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
	SetCurrentDirectoryW(game.c_str());
	const std::wstring vault = local + L"\\MeowyClientPack\\vault";

	data_sandbox::Recover();
	Check(Exists(data + L"\\zrms\\settings.txt"), "start: nothing moved before the first menu frame");

	data_sandbox::Leave();
	Check(!Exists(data + L"\\zrms") && !Exists(data + L"\\193_243_190_11_27041_nexus_inventory_slots.txt"), "menu: data/ emptied");
	Check(Exists(vault + L"\\before-sandbox\\zrms\\settings.txt"), "menu: old data kept in before-sandbox");

	Touch(data + L"\\menu_file.txt");
	data_sandbox::Enter("1.2.3.4:27015");
	Check(!Exists(data + L"\\menu_file.txt"), "server A: menu's file not in data/");
	Check(Exists(vault + L"\\menu\\menu_file.txt"), "server A: menu's file in the vault");
	Touch(data + L"\\a_file.txt");

	data_sandbox::Enter("5.6.7.8:27015");
	Check(!Exists(data + L"\\a_file.txt"), "server B: A's file not in data/");
	Check(Exists(vault + L"\\1.2.3.4_27015\\a_file.txt"), "server B: A's file in A's vault");
	Touch(data + L"\\b_file.txt");

	data_sandbox::Enter("1.2.3.4:27015");
	Check(Exists(data + L"\\a_file.txt") && !Exists(data + L"\\b_file.txt"), "back to A: A's file back, B's gone");

	// A crash while on A: the next start (a new process; here the cached state
	// is simply what the file says) must put A's files away and the menu's back.
	data_sandbox::Recover();
	Check(Exists(data + L"\\menu_file.txt") && !Exists(data + L"\\a_file.txt"), "recover: menu's files back, A's away");
	Check(Exists(vault + L"\\1.2.3.4_27015\\a_file.txt"), "recover: A's file kept");

	std::printf("%s\n", g_failed ? "FAILED" : "all passed");
	return g_failed ? 1 : 0;
}
