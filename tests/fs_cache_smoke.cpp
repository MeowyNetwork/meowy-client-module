// The missing-file cache's rules, no game: which paths it answers for, which
// folders it never lists, and what it says against real folders.
//   fs_cache_smoke <empty scratch folder>
// Exit code 0 when every check passes.

#include "../src/fs_cache.h"

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

static bool SplitIs(const char* path, const char* dir, const char* name)
{
	std::string d, n;
	return fs_cache::detail::Split(path, d, n) && d == dir && n == name;
}

static bool NotSplit(const char* path)
{
	std::string d, n;
	return !fs_cache::detail::Split(path, d, n);
}

static void Put(const std::string& path)
{
	HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	CloseHandle(h);
}

static FILETIME FolderTime(const std::string& dir)
{
	WIN32_FILE_ATTRIBUTE_DATA a = {};
	GetFileAttributesExA(dir.c_str(), GetFileExInfoStandard, &a);
	return a.ftLastWriteTime;
}

// A folder's last-write time: `exact`, or `secondsAgo` before now.
static void SetFolderTime(const std::string& dir, int secondsAgo, const FILETIME* exact = nullptr)
{
	FILETIME ft;
	if (exact)
	{
		ft = *exact;
	}
	else
	{
		GetSystemTimeAsFileTime(&ft);
		ULARGE_INTEGER t;
		t.LowPart = ft.dwLowDateTime;
		t.HighPart = ft.dwHighDateTime;
		t.QuadPart -= static_cast<unsigned long long>(secondsAgo) * 10000000ULL;
		ft.dwLowDateTime = t.LowPart;
		ft.dwHighDateTime = t.HighPart;
	}
	HANDLE h = CreateFileA(dir.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	SetFileTime(h, nullptr, nullptr, &ft);
	CloseHandle(h);
}

// Listings reused from earlier joins so far (from the status line).
static long long Reused()
{
	const std::string d = fs_cache::Describe();
	const size_t at = d.find(" folder listings (");
	return at == std::string::npos ? -1 : std::atoll(d.c_str() + at + 18);
}

int main(int argc, char** argv)
{
	using namespace fs_cache::detail;

	// Paths
	Check("plain path", SplitIs("C:\\Games\\GarrysMod\\garrysmod\\materials\\x.vmt", "c:\\games\\garrysmod\\garrysmod\\materials", "x.vmt"));
	Check("forward slashes, mixed case", SplitIs("C:/Games\\GarrysMod/materials/Models/A.VTF", "c:\\games\\garrysmod\\materials\\models", "a.vtf"));
	Check("doubled separators", SplitIs("c:\\a\\\\b//c.txt", "c:\\a\\b", "c.txt"));
	Check("file in a drive's root", SplitIs("d:\\x.txt", "d:", "x.txt"));
	Check("relative path refused", NotSplit("materials\\x.vmt"));
	Check("UNC refused", NotSplit("\\\\server\\share\\x.vmt"));
	Check("dot-dot refused", NotSplit("c:\\a\\..\\b\\x.vmt"));
	Check("dot refused", NotSplit("c:\\a\\.\\x.vmt"));
	Check("trailing dot refused", NotSplit("c:\\a\\x.vmt."));
	Check("trailing space refused", NotSplit("c:\\a\\x.vmt "));
	Check("short name refused", NotSplit("c:\\progra~1\\x.vmt"));
	Check("stream refused", NotSplit("c:\\a\\x.vmt:zone"));
	Check("wildcard refused", NotSplit("c:\\a\\*.vmt"));
	Check("non-ASCII refused", NotSplit("c:\\users\\j\xc3\xb6rg\\x.vmt"));
	Check("folder path refused", NotSplit("c:\\a\\b\\"));
	Check("null refused", NotSplit(nullptr));

	// Folders never listed
	Check("data excluded", Excluded("c:\\steam\\steamapps\\common\\garrysmod\\garrysmod\\data"));
	Check("data subfolder excluded", Excluded("c:\\x\\garrysmod\\data\\meowy"));
	Check("download excluded", Excluded("c:\\x\\garrysmod\\download\\materials"));
	Check("cache excluded", Excluded("c:\\x\\garrysmod\\cache\\lua"));
	Check("workshop content excluded", Excluded("c:\\steam\\steamapps\\workshop\\content\\4000"));
	Check("materials not excluded", !Excluded("c:\\x\\garrysmod\\materials\\models"));
	Check("a look-alike not excluded", !Excluded("c:\\x\\garrysmod\\datapack\\x"));

	// Real folders
	char full[MAX_PATH];
	GetFullPathNameA(argc > 1 ? argv[1] : ".", MAX_PATH, full, nullptr);
	const std::string root = std::string(full) + "\\fscache";
	CreateDirectoryA(root.c_str(), nullptr);
	CreateDirectoryA((root + "\\materials").c_str(), nullptr);
	Put(root + "\\materials\\Here.VMT");
	std::string dir, name;
	Split((root + "\\materials\\here.vmt").c_str(), dir, name);
	Check("a file that's there: present (any case)", Lookup(dir, name) == Answer::Present);
	Split((root + "\\materials\\gone.vmt").c_str(), dir, name);
	Check("a file that isn't: absent", Lookup(dir, name) == Answer::Absent);
	Check("one listing for the folder", Folders() == 1);
	Split((root + "\\nofolder\\x.vmt").c_str(), dir, name);
	Check("a folder that isn't there: absent", Lookup(dir, name) == Answer::Absent);
	Check("its absence cached too", Folders() == 2);

	// A file written later is seen after the folder is dropped.
	Put(root + "\\materials\\gone.vmt");
	Split((root + "\\materials\\gone.vmt").c_str(), dir, name);
	Check("stale until dropped (why writes drop the folder)", Lookup(dir, name) == Answer::Absent);
	Invalidate(dir);
	Check("present once the folder is listed again", Lookup(dir, name) == Answer::Present);
	// Across joins. A folder that changed moments ago isn't trusted past this
	// join: the clock can be too coarse for a second change to move its time.
	const std::string materials = root + "\\materials";
	NewJoin();
	long long before = Reused();
	Check("next join: a folder that changed just now is listed again", Lookup(dir, name) == Answer::Present && Reused() == before);
	// The same-tick case itself: listed, a file added, the folder's time put back
	// to what it was (as a coarse clock would leave it): still seen next join.
	SetFolderTime(materials, 0);
	Invalidate(dir);
	Lookup(dir, name);
	FILETIME same = FolderTime(materials);
	Put(root + "\\materials\\sametick.vmt");
	SetFolderTime(materials, 0, &same);
	std::string dirS, sameTick;
	Split((root + "\\materials\\sametick.vmt").c_str(), dirS, sameTick);
	NewJoin();
	Check("next join: a file added in the same clock tick is seen", Lookup(dirS, sameTick) == Answer::Present);
	DeleteFileA((root + "\\materials\\sametick.vmt").c_str());
	// A settled folder (written a minute ago) is reused in the next join...
	SetFolderTime(materials, 60);
	Invalidate(dir);
	Lookup(dir, name);
	NewJoin();
	before = Reused();
	Check("next join: an unchanged, settled folder answers", Lookup(dir, name) == Answer::Present);
	Check("next join: reused, not listed again", Reused() == before + 1);
	// ...and listed again once a name in it changes.
	Put(root + "\\materials\\added.vmt");
	std::string dir2, added;
	Split((root + "\\materials\\added.vmt").c_str(), dir2, added);
	Check("same join: a file added since isn't seen yet", Lookup(dir2, added) == Answer::Absent);
	NewJoin();
	Check("next join: the folder changed, so it's listed again and the file is seen", Lookup(dir2, added) == Answer::Present);
	Split((root + "\\later\\x.vmt").c_str(), dir, name);
	Check("a folder that isn't there yet", Lookup(dir, name) == Answer::Absent);
	CreateDirectoryA((root + "\\later").c_str(), nullptr);
	Put(root + "\\later\\x.vmt");
	NewJoin();
	Check("next join: the folder appeared, so it's listed and the file is seen", Lookup(dir, name) == Answer::Present);
	DeleteFileA((root + "\\later\\x.vmt").c_str());
	RemoveDirectoryA((root + "\\later").c_str());
	DeleteFileA((root + "\\materials\\added.vmt").c_str());

	Clear();
	Check("cleared", Folders() == 0);

	DeleteFileA((root + "\\materials\\Here.VMT").c_str());
	DeleteFileA((root + "\\materials\\gone.vmt").c_str());
	RemoveDirectoryA((root + "\\materials").c_str());
	RemoveDirectoryA(root.c_str());
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
