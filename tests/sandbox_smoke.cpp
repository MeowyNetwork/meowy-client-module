// sandbox_smoke: runs data_sandbox against a throwaway GarrysMod folder.
//   sandbox_smoke <empty scratch folder on an NTFS drive>
// Builds <scratch>\GarrysMod\garrysmod\data, points LOCALAPPDATA at
// <scratch>\local, and walks: first start -> menu -> server A -> server B
// (with A's file held open, as GMod holds a .gma it mounted) -> crash ->
// recover, then each layout a pre-1.6.1 pack can leave behind.
#include "../src/data_sandbox.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		++g_failed;
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool IsLink(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
static void Put(const std::wstring& p, const char* text = "x")
{
	size_t slash = p.find_last_of(L'\\');
	for (size_t i = p.find(L'\\', 3); i != std::wstring::npos && i <= slash; i = p.find(L'\\', i + 1))
		CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
	HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return;
	DWORD wrote = 0;
	WriteFile(h, text, static_cast<DWORD>(strlen(text)), &wrote, nullptr);
	CloseHandle(h);
}
static std::string Text(const std::wstring& p)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return "<missing>";
	char buf[256];
	DWORD got = 0;
	ReadFile(h, buf, sizeof(buf), &got, nullptr);
	CloseHandle(h);
	return std::string(buf, got);
}
static std::vector<std::wstring> Entries(const std::wstring& dir)
{
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW f;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do
		if (wcscmp(f.cFileName, L".") && wcscmp(f.cFileName, L".."))
			out.push_back(f.cFileName);
	while (FindNextFileW(h, &f));
	FindClose(h);
	return out;
}
// Deletes a scratch tree; links are removed, never followed.
static void Wipe(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	if (a == INVALID_FILE_ATTRIBUTES)
		return;
	if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT))
		for (const std::wstring& e : Entries(p))
			Wipe(p + L"\\" + e);
	if (a & FILE_ATTRIBUTE_DIRECTORY)
		RemoveDirectoryW(p.c_str());
	else
		DeleteFileW(p.c_str());
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2)
	{
		std::printf("usage: sandbox_smoke <empty scratch folder on an NTFS drive>\n");
		return 2;
	}
	wchar_t full[MAX_PATH];
	GetFullPathNameW(argv[1], MAX_PATH, full, nullptr);
	const std::wstring root = full;
	const std::wstring game = root + L"\\GarrysMod", garrysmod = game + L"\\garrysmod", data = garrysmod + L"\\data";
	const std::wstring local = root + L"\\local", vault = local + L"\\MeowyClientPack\\vault";
	const std::wstring A = vault + L"\\1.2.3.4_27015", B = vault + L"\\5.6.7.8_27015", menu = vault + L"\\menu";
	auto fresh = [&]() {
		Wipe(data);
		Wipe(data + L".meowy-link");
		Wipe(local);
		CreateDirectoryW(root.c_str(), nullptr);
		CreateDirectoryW(game.c_str(), nullptr);
		CreateDirectoryW(garrysmod.c_str(), nullptr);
		CreateDirectoryW(data.c_str(), nullptr);
		CreateDirectoryW(local.c_str(), nullptr);
	};
	fresh();
	SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
	SetCurrentDirectoryW(game.c_str());

	// --- First start: data/ was never sandboxed --------------------------------
	Put(data + L"\\zrms\\settings.txt", "zrms");
	Put(data + L"\\193_243_190_11_27041_nexus_inventory_slots.txt");
	std::string said = data_sandbox::Recover();
	std::printf("     recover said: %s\n", said.c_str());
	Check(IsLink(data), "first start: data/ is a link");
	Check(Text(vault + L"\\before-sandbox\\zrms\\settings.txt") == "zrms", "first start: old data kept in before-sandbox");
	Check(Entries(data).empty(), "first start: data/ shows the (empty) menu folder");
	Check(Text(vault + L"\\active") == "linked", "first start: active says linked (for older uninstallers)");
	Check(!Exists(data + L".meowy-link"), "first start: no temporary link left");

	// --- Menu -> A -> B with A's file held open -> A ------------------------------
	data_sandbox::Leave();
	Put(data + L"\\menu_file.txt", "menu");
	Check(Text(menu + L"\\menu_file.txt") == "menu", "menu: written through the link into vault\\menu");

	data_sandbox::Enter("1.2.3.4:27015");
	Check(!Exists(data + L"\\menu_file.txt"), "server A: menu's file not in data/");
	Put(data + L"\\a.gma", "a");
	Check(Text(A + L"\\a.gma") == "a", "server A: its file lands in A's folder");

	// GMod keeps a .gma it mounted open (read, no delete sharing) for the rest of the session.
	HANDLE held = CreateFileW((data + L"\\a.gma").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	Check(held != INVALID_HANDLE_VALUE, "server A: a.gma held open");
	data_sandbox::Enter("5.6.7.8:27015");
	Check(!Exists(data + L"\\a.gma") && Entries(data).empty(), "server B with A's file open: switched, A's file not shown");
	Put(data + L"\\b_file.txt", "b");
	Check(Text(B + L"\\b_file.txt") == "b" && !Exists(A + L"\\b_file.txt"), "server B: B's file in B's folder, not A's");
	data_sandbox::Leave();
	Check(Text(data + L"\\menu_file.txt") == "menu" && !Exists(data + L"\\b_file.txt"), "menu again (A's file still open): menu's files only");
	CloseHandle(held);
	Check(Text(A + L"\\a.gma") == "a", "A's open file stayed in A's folder");

	data_sandbox::Enter("1.2.3.4:27015");
	Check(Exists(data + L"\\a.gma") && !Exists(data + L"\\b_file.txt"), "back to A: A's file back, B's not shown");

	// "..": resolved in the path text, so it leads back to garrysmod\, never into the vault.
	Put(data + L"\\..\\dotdot_probe.txt");
	Check(Exists(garrysmod + L"\\dotdot_probe.txt") && !Exists(vault + L"\\dotdot_probe.txt"), "data\\.. is garrysmod\\, not the vault");
	DeleteFileW((garrysmod + L"\\dotdot_probe.txt").c_str());

	// A crash while on A: the next start links data/ back to the menu's folder.
	data_sandbox::Recover();
	Check(Text(data + L"\\menu_file.txt") == "menu" && !Exists(data + L"\\a.gma"), "recover after a crash on A: menu's files");
	Check(Exists(A + L"\\a.gma"), "recover: A's file kept in A's folder");

	// Keys come from the engine's address: anything but a real ip:port gets the fixed "other" folder.
	for (const char* bad : { ".", "..", "evil", "1.2.3.4", "1.2.3.4:27015/..", "999.1.1.1:27015", "..\\..\\x:1" })
		data_sandbox::Enter(bad);
	Check(!Exists(data + L"\\menu_file.txt") && Exists(vault + L"\\other"), "odd address: the fixed other folder");
	{
		bool onlyKnown = true;
		for (const std::wstring& e : Entries(vault))
			onlyKnown = onlyKnown && (e == L"active" || e == L"menu" || e == L"before-sandbox" || e == L"other" ||
				e == L"1.2.3.4_27015" || e == L"5.6.7.8_27015");
		Check(onlyKnown && !Exists(local + L"\\MeowyClientPack\\x") && !Exists(root + L"\\x"), "odd address: no folder named from it");
	}
	data_sandbox::Enter("p2p:76561198000000000");
	Put(data + L"\\p2p_file.txt");
	data_sandbox::Enter("p2p:76561198000000001");
	Check(Exists(data + L"\\p2p_file.txt") && Exists(vault + L"\\p2p\\p2p_file.txt"), "p2p: every P2P host shares one folder");
	data_sandbox::EnterUnknown();
	Check(!Exists(data + L"\\p2p_file.txt") && !Exists(data + L"\\menu_file.txt"), "unknown server: the other folder, not the menu's");
	data_sandbox::Leave();
	Check(Text(data + L"\\menu_file.txt") == "menu", "menu: back");
	Check(data_sandbox::Describe().find("links to menu") != std::string::npos, "describe: links to menu");

	// A start that died right after linking (or failed to write the marker):
	// the next start's Recover puts "linked" back, which older uninstallers read.
	DeleteFileW((vault + L"\\active").c_str());
	data_sandbox::Recover();
	Check(Text(vault + L"\\active") == "linked", "recover: a missing marker is written again");
	Put(vault + L"\\active", "menu");
	data_sandbox::Recover();
	Check(Text(vault + L"\\active") == "linked", "recover: a wrong marker is put right");

	// A staging folder an earlier start left behind is merged into its owner's folder.
	Put(vault + L"\\menu.incoming\\staged.txt", "staged");
	Put(vault + L"\\menu.incoming-2\\staged2.txt", "staged2");
	Put(vault + L"\\menu.incomingX\\not_staging.txt", "x");
	data_sandbox::Recover();
	Check(Text(data + L"\\staged.txt") == "staged" && Text(data + L"\\staged2.txt") == "staged2",
		"recover: leftover staging folders merged into the menu's files");
	Check(!Exists(vault + L"\\menu.incoming") && !Exists(vault + L"\\menu.incoming-2"), "recover: staging folders gone");
	Check(Exists(vault + L"\\menu.incomingX\\not_staging.txt"), "recover: a folder that only looks like one is left alone");
	Wipe(vault + L"\\menu.incomingX");

	// --- Pre-1.6.1 layouts ----------------------------------------------------------
	// After a crash on A with the old pack: data/ holds A's files, the vault the rest.
	fresh();
	Put(data + L"\\a.txt", "a");
	Put(vault + L"\\active", "1.2.3.4_27015");
	Put(menu + L"\\m.txt", "m");
	Put(vault + L"\\before-sandbox\\old.txt", "old");
	data_sandbox::Recover();
	Check(IsLink(data) && Text(data + L"\\m.txt") == "m" && !Exists(data + L"\\a.txt"), "old crash on A: data/ links to the menu's files");
	Check(Text(A + L"\\a.txt") == "a" && Text(vault + L"\\before-sandbox\\old.txt") == "old", "old crash on A: A's and the old files kept");

	// The old pack died moving A's files out (phase 1): some in data/, the rest already in A's folder.
	fresh();
	Put(data + L"\\x1.txt", "1");
	Put(A + L"\\x2.txt", "2");
	Put(vault + L"\\active", "switching:1.2.3.4_27015>menu:1");
	data_sandbox::Recover();
	Check(Text(A + L"\\x1.txt") == "1" && Text(A + L"\\x2.txt") == "2" && IsLink(data), "old phase 1: A's files together, data/ linked");
	Check(!Exists(A + L".incoming"), "old phase 1: nothing left in a staging folder");

	// A clash (the old pack overwrote; this keeps both, data/'s newer copy under the name).
	fresh();
	Put(data + L"\\shared.txt", "new");
	Put(data + L"\\sub\\deep.txt", "new deep");
	Put(A + L"\\shared.txt", "old");
	Put(A + L"\\sub\\other.txt", "kept");
	Put(vault + L"\\active", "1.2.3.4_27015");
	data_sandbox::Recover();
	Check(Text(A + L"\\shared.txt") == "new" && Text(A + L"\\shared.older.txt") == "old", "clash: both kept, the newer under the name");
	Check(Text(A + L"\\sub\\deep.txt") == "new deep" && Text(A + L"\\sub\\other.txt") == "kept", "clash: folders merged");

	// The link was removed by hand and GMod made a plain data/ again.
	fresh();
	Put(data + L"\\since.txt", "since");
	Put(vault + L"\\active", "linked");
	Put(menu + L"\\m.txt", "m");
	data_sandbox::Recover();
	Check(Text(data + L"\\since.txt") == "since" && Text(data + L"\\m.txt") == "m", "link removed by hand: new files go with the menu's");

	// A start that died between making the temporary link and renaming data/: the leftover goes.
	fresh();
	Put(data + L"\\f.txt", "f");
	{
		wchar_t cmd[2048];
		swprintf_s(cmd, L"cmd /c mklink /J \"%s.meowy-link\" \"%s\" >nul", data.c_str(), menu.c_str());
		CreateDirectoryW(vault.c_str(), nullptr);
		CreateDirectoryW(menu.c_str(), nullptr);
		_wsystem(cmd);
	}
	data_sandbox::Recover();
	Check(IsLink(data) && Text(vault + L"\\before-sandbox\\f.txt") == "f" && !Exists(data + L".meowy-link"), "leftover temporary link: replaced, converted");
	// Something of the player's with that name: never touched, nothing moved.
	fresh();
	Put(data + L"\\f.txt", "f");
	Put(data + L".meowy-link\\keep.txt", "keep");
	said = data_sandbox::Recover();
	std::printf("     recover said: %s\n", said.c_str());
	Check(!IsLink(data) && Text(data + L"\\f.txt") == "f" && Text(data + L".meowy-link\\keep.txt") == "keep", "a folder with files in the way: nothing moved");
	Wipe(data + L".meowy-link");

	// A file in data/ open elsewhere at start-up: nothing moves, nothing is lost; the next start converts.
	fresh();
	Put(data + L"\\open.txt", "open");
	Put(data + L"\\other.txt", "other");
	HANDLE open = CreateFileW((data + L"\\open.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	said = data_sandbox::Recover();
	std::printf("     recover said: %s\n", said.c_str());
	Check(!IsLink(data) && Text(data + L"\\other.txt") == "other" && !Exists(data + L".meowy-link"), "file open at start: data/ untouched");
	data_sandbox::Enter("1.2.3.4:27015");
	Check(Text(data + L"\\other.txt") == "other", "file open at start: no switching that session");
	CloseHandle(open);
	data_sandbox::Recover();
	Check(IsLink(data) && Text(vault + L"\\before-sandbox\\open.txt") == "open", "next start: converted");

	// Someone else's link (data/ on another drive, made by the player): left alone.
	fresh();
	RemoveDirectoryW(data.c_str());
	const std::wstring elsewhere = root + L"\\elsewhere";
	CreateDirectoryW(elsewhere.c_str(), nullptr);
	Put(elsewhere + L"\\mine.txt", "mine");
	wchar_t cmd[2048];
	swprintf_s(cmd, L"cmd /c mklink /J \"%s\" \"%s\" >nul", data.c_str(), elsewhere.c_str());
	_wsystem(cmd);
	data_sandbox::Recover();
	data_sandbox::Enter("1.2.3.4:27015");
	Check(Text(data + L"\\mine.txt") == "mine" && !Exists(A), "the player's own link: never touched");
	Check(data_sandbox::Describe().rfind("off:", 0) == 0, "the player's own link: sandbox off");
	Wipe(data);
	Wipe(elsewhere);

	std::printf("%s\n", g_failed ? "FAILED" : "all passed");
	return g_failed ? 1 : 0;
}
