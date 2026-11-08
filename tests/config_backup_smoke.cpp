// Copies of config.cfg, no game:
//   config_backup_smoke <empty scratch folder>
// A copy per change, none when unchanged, the newest 10 kept, names checked,
// and a copy written back as meowy_restore.cfg. Exit code 0 when every check passes.

#include "../src/config_backup.h"
#include "../src/file_util.h"

#include <cstdio>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int g_failures = 0, g_count = 0;

static void Check(const char* name, bool ok)
{
	++g_count;
	if (!ok)
	{
		++g_failures;
		std::printf("FAIL %s\n", name);
	}
}

static std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: config_backup_smoke <empty scratch folder>\n");
		return 2;
	}
	// A folder of its own per bitness, emptied first: both runs can share one scratch folder.
	const std::wstring root = Widen(argv[1]) + (sizeof(void*) == 8 ? L"\\config_backup_smoke64" : L"\\config_backup_smoke32");
	CreateDirectoryW(root.c_str(), nullptr);
	const std::wstring cfgDir = root + L"\\cfg", dir = root + L"\\backups", cfg = cfgDir + L"\\config.cfg";
	CreateDirectoryW(cfgDir.c_str(), nullptr);
	for (const std::wstring& d : { cfgDir, dir })
	{
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &f);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		do
		{
			if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				DeleteFileW((d + L"\\" + f.cFileName).c_str());
		} while (FindNextFileW(h, &f));
		FindClose(h);
	}
	std::string r;

	// Names.
	Check("valid name", config_backup::ValidName("config-20261107-203512.cfg"));
	Check("no path in a name", !config_backup::ValidName("..\\config-20261107-203512.cfg"));
	Check("no letters in the stamp", !config_backup::ValidName("config-2026110x-203512.cfg"));
	Check("no other extension", !config_backup::ValidName("config-20261107-203512.txt"));

	// Nothing to copy yet.
	Check("missing config.cfg: no copy", !config_backup::Backup(cfg, dir, "20261107-100000", r));

	file_util::WriteAtomic(cfg, "unbindall\nbind \"w\" \"+forward\"\nmat_specular \"1\"\n");
	Check("first start: copied", config_backup::Backup(cfg, dir, "20261107-100000", r) && r == "copied");
	Check("same file next start: unchanged", config_backup::Backup(cfg, dir, "20261107-110000", r) && r == "unchanged");
	Check("one copy", config_backup::List(dir).size() == 1);

	const std::string changed = "unbindall\nbind \"w\" \"+forward\"\nmat_specular \"0\"\n";
	file_util::WriteAtomic(cfg, changed);
	Check("changed: copied", config_backup::Backup(cfg, dir, "20261107-120000", r) && r == "copied");
	auto list = config_backup::List(dir);
	Check("two copies, newest first", list.size() == 2 && list[0].name == "config-20261107-120000.cfg");
	Check("its time, readable", !list.empty() && list[0].when == "2026-11-07 12:00:00");
	Check("its size", !list.empty() && list[0].size == changed.size());
	Check("an older stamp than the newest copy: refused", !config_backup::Backup(
		(file_util::WriteAtomic(cfg, "changed again\n"), cfg), dir, "20261107-110000", r));

	// The newest 10 kept.
	for (int i = 0; i < 12; ++i)
	{
		char stamp[32];
		std::snprintf(stamp, sizeof stamp, "20261108-%06d", 100000 + i);
		file_util::WriteAtomic(cfg, "version " + std::to_string(i) + "\n");
		config_backup::Backup(cfg, dir, stamp, r);
	}
	list = config_backup::List(dir);
	Check("10 kept", list.size() == config_backup::kKeep);
	Check("the newest kept", !list.empty() && list[0].name == "config-20261108-100011.cfg");
	Check("the oldest gone", list.size() == 10 && list[9].name == "config-20261108-100002.cfg");

	// A file that isn't ours in the folder: left out.
	file_util::WriteAtomic(dir + L"\\config-notes.cfg", "x");
	Check("other files left out", config_backup::List(dir).size() == config_backup::kKeep);

	// Back as meowy_restore.cfg.
	Check("restore", config_backup::Restore(dir, "config-20261108-100005.cfg", cfgDir, r) && r == "meowy_restore.cfg");
	Check("restore wrote that copy", file_util::ReadSmall(cfgDir + L"\\meowy_restore.cfg", 1024) == "version 5\n");
	Check("restore: only our names", !config_backup::Restore(dir, "..\\..\\x.cfg", cfgDir, r));
	Check("restore: a copy that's gone", !config_backup::Restore(dir, "config-20261108-100000.cfg", cfgDir, r));

	// Removed once the game has run it (or left behind): only meowy_restore.cfg.
	Check("remove: done", config_backup::RemoveRestore(cfgDir));
	Check("remove: the file's gone", GetFileAttributesW((cfgDir + L"\\meowy_restore.cfg").c_str()) == INVALID_FILE_ATTRIBUTES);
	Check("remove: config.cfg stays", GetFileAttributesW(cfg.c_str()) != INVALID_FILE_ATTRIBUTES);
	Check("remove: nothing there is fine", config_backup::RemoveRestore(cfgDir));
	Check("remove: no cfg folder is fine", config_backup::RemoveRestore(root + L"\\no-such-folder"));

	// The game's folder from a module's path: only from a lua\bin.
	Check("game folder: the installed copy", config_backup::GameDirOf(
		L"C:\\Steam\\steamapps\\common\\GarrysMod\\garrysmod\\lua\\bin\\gmcl_meowy_win64.dll") == L"C:\\Steam\\steamapps\\common\\GarrysMod\\garrysmod");
	Check("game folder: the start-up stub, any case", config_backup::GameDirOf(
		L"D:\\Games\\GarrysMod\\garrysmod\\LUA\\Bin\\gmsv_meowy_boot_win32.dll") == L"D:\\Games\\GarrysMod\\garrysmod");
	Check("game folder: forward slashes", config_backup::GameDirOf(L"C:/GarrysMod/garrysmod/lua/bin/gmcl_meowy_win64.dll") == L"C:/GarrysMod/garrysmod");
	Check("game folder: not from an update", config_backup::GameDirOf(
		L"C:\\Users\\a\\AppData\\Local\\MeowyClientPack\\bin\\1.7.1\\gmcl_meowy_win64.dll").empty());
	Check("game folder: not from a folder only ending in lua\\bin", config_backup::GameDirOf(L"C:\\x\\notlua\\bin\\a.dll").empty());
	Check("game folder: no folder", config_backup::GameDirOf(L"gmcl_meowy_win64.dll").empty());
	Check("game folder: nothing", config_backup::GameDirOf(L"").empty());

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
