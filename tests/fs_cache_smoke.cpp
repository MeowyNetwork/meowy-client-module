// The missing-file cache's rules, no game: which paths it answers for, which
// folders it never lists, and what it says against real folders.
//   fs_cache_smoke <empty scratch folder>
// Exit code 0 when every check passes.

#include "../src/fs_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

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

// The number after `after` in the status line.
static long long Count(const char* after)
{
	const std::string d = fs_cache::Describe();
	const size_t at = d.find(after);
	return at == std::string::npos ? -1 : std::atoll(d.c_str() + at + std::strlen(after));
}

// Listings reused from earlier joins so far (from the status line).
static long long Reused()
{
	return Count(" folder listings (");
}

// Folders listed so far.
static long long Listed()
{
	return Count(" opens answered \"not there\" from ");
}

// Folders answered absent from the folder above, with no listing.
static long long Inferred()
{
	return Count(" reused from earlier joins), ");
}

// What the cache says about a full path.
static fs_cache::detail::Answer Ask(const std::string& path)
{
	std::string d, n;
	if (!fs_cache::detail::Split(path.c_str(), d, n))
		return fs_cache::detail::Answer::Unknown;
	return fs_cache::detail::Lookup(d, n);
}

static std::string DirOf(const std::string& path)
{
	std::string d, n;
	fs_cache::detail::Split(path.c_str(), d, n);
	return d;
}

// --- Split as it was before the stack-buffer one, for the parity check ---------------
static bool OldLowerAscii(std::string& s)
{
	for (char& c : s)
	{
		const unsigned char u = static_cast<unsigned char>(c);
		if (u >= 0x80)
			return false;
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return true;
}

static bool OldSplit(const char* path, std::string& dir, std::string& name)
{
	if (!path)
		return false;
	const size_t n = std::strlen(path);
	if (n < 4 || n >= 260)
		return false;
	const char d = path[0];
	if (!((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z')) || path[1] != ':' || (path[2] != '\\' && path[2] != '/'))
		return false;
	std::string out = std::string(1, path[0]) + ":";
	std::string component;
	std::string last;
	for (size_t i = 3; i <= n; ++i)
	{
		const char c = i < n ? path[i] : '\\';
		if (c == '\\' || c == '/')
		{
			if (component.empty())
				continue;   // a doubled separator
			if (component == "." || component == ".." || component.back() == '.' || component.back() == ' ')
				return false;
			if (!last.empty())
				out += "\\" + last;
			last = component;
			component.clear();
			continue;
		}
		// wildcards, streams, short names, line breaks: not for the cache
		if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == ':' || c == '~' ||
			c == '\r' || c == '\n' || static_cast<unsigned char>(c) < 0x20)
			return false;
		component += c;
	}
	if (last.empty() || path[n - 1] == '\\' || path[n - 1] == '/')
		return false;
	if (!OldLowerAscii(out) || !OldLowerAscii(last))
		return false;
	dir = out;
	name = last;
	return true;
}

// The same answer, folder and name from both.
static bool SameSplit(const char* path)
{
	std::string d1, n1, d2, n2;
	const bool a = OldSplit(path, d1, n1);
	const bool b = fs_cache::detail::Split(path, d2, n2);
	return a == b && (!a || (d1 == d2 && n1 == n2));
}

// --- Test hooks ------------------------------------------------------------------------
static std::string g_hookFile;       // made by the hook, once
static std::string g_hookDrop;       // dropped by the hook, once ("" for none)
static bool g_hookSleep = false;     // and then 2.1 s pass, as in a slow listing
static fs_cache::detail::TestPoint g_hookAt = fs_cache::detail::TestPoint::Listed;
static bool g_hookDone = false;

static void Hook(fs_cache::detail::TestPoint at)
{
	if (g_hookDone || at != g_hookAt)
		return;
	g_hookDone = true;
	if (!g_hookFile.empty())
		Put(g_hookFile);
	if (!g_hookDrop.empty())
		fs_cache::detail::Invalidate(g_hookDrop);
	if (g_hookSleep)
		Sleep(2100);
}

static void SetHook(fs_cache::detail::TestPoint at, const std::string& file, const std::string& drop, bool sleep)
{
	g_hookAt = at;
	g_hookFile = file;
	g_hookDrop = drop;
	g_hookSleep = sleep;
	g_hookDone = false;
	fs_cache::detail::SetTestHook(&Hook);
}

// A folder's listing denied to everyone (true), or allowed again.
static bool DenyListing(const std::string& dir, bool deny)
{
	PSECURITY_DESCRIPTOR sd = nullptr;
	const char* sddl = deny ? "D:P(D;;0x1;;;WD)(A;OICI;FA;;;WD)" : "D:P(A;OICI;FA;;;WD)";
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, nullptr))
		return false;
	const BOOL ok = SetFileSecurityA(dir.c_str(), DACL_SECURITY_INFORMATION, sd);
	LocalFree(sd);
	return ok != FALSE;
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

	// The stack-buffer Split answers exactly as the old one did: every refusal
	// above and more, then a few thousand random paths.
	{
		static const char* const kCorpus[] = {
			nullptr, "", "c", "c:", "c:\\", "c:\\a", "c:\\ab", "c:\\\\\\\\", "c://///x", "C:\\A\\B\\", "c:/a//b",
			"1:\\a\\b", "c;\\a\\b", "c:a\\b\\c", "c:\\a\\b.", "c:\\a. \\b", "c:\\a\\ b", "c:\\ a\\b", "c:\\a\\..\\b",
			"c:\\a\\...\\b", "c:\\a\\.b", "c:\\.a\\b", "c:\\a\\b\x01", "c:\\a\\b\t", "c:\\a\\\xff", "c:\\\xc3\xa9\\b",
			"c:\\a\\b\\c:d", "c:\\a\\b~1\\c", "c:\\a\\b?", "c:\\a\\b|c", "c:\\a\\<b>", "c:\\a\\\"b\"", "c:\\a\\b\r\n",
			"Z:/Games/GarrysMod/x.VMT", "d:\\x.txt", "d:\\\\x.txt", "\\\\server\\share\\x.vmt", "materials\\x.vmt",
			"c:\\a\\..", "c:\\a\\.", "c:\\a\\b ", "c:\\a \\b", "c:\\progra~1\\x.vmt", "c:\\a\\x.vmt:zone",
		};
		int mismatches = 0;
		for (size_t i = 0; i < sizeof(kCorpus) / sizeof(kCorpus[0]); ++i)
		{
			if (!SameSplit(kCorpus[i]))
			{
				++mismatches;
				std::printf("  Split differs on fixed path %zu\n", i);
			}
		}
		// Lengths around the limit: 259 is the longest it answers for.
		for (size_t len = 250; len <= 266; ++len)
		{
			std::string p = "c:\\";
			while (p.size() < len)
				p += (p.size() % 9 == 0) ? '\\' : 'a';
			p.back() = 'x';
			if (!SameSplit(p.c_str()))
			{
				++mismatches;
				std::printf("  Split differs at length %zu\n", len);
			}
		}
		// Half the paths from separators, dots, spaces and letters only, so
		// plenty get through; the rest with everything it refuses mixed in.
		static const char kAlphabet[] = "\\\\//..  aaaBBBzzz~:*?|\"<>\t\x80\xc3\xff";
		const size_t kPlain = 17;   // the first 17 characters above
		unsigned long long seed = 0x9E3779B97F4A7C15ULL;
		auto next = [&seed]() {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			return static_cast<unsigned>(seed >> 33);
		};
		const int kRandom = 5000;
		int accepted = 0;
		for (int i = 0; i < kRandom; ++i)
		{
			std::string p;
			const unsigned prefix = next() % 4;
			if (prefix == 0)
				p = "c:\\";
			else if (prefix == 1)
				p = "Z:/";
			else if (prefix == 2)
				p = "q:\\Games\\";
			const unsigned shape = next() % 10;
			const size_t len = shape == 0 ? 245 + next() % 25 : next() % 40;
			const size_t letters = (i % 2) ? kPlain : sizeof(kAlphabet) - 1;
			while (p.size() < len)
				p += kAlphabet[next() % letters];
			std::string d, n;
			if (OldSplit(p.c_str(), d, n))
				++accepted;
			if (!SameSplit(p.c_str()))
			{
				if (mismatches < 10)
					std::printf("  Split differs on random path %d\n", i);
				++mismatches;
			}
		}
		std::printf("Split parity: %d random paths, %d of them answered for\n", kRandom, accepted);
		Check("Split: the same answers as before on every path", mismatches == 0);
		Check("Split parity: some random paths answered for", accepted > 0);
	}

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

	// Absent from the folder above, with no listing.
	using fs_cache::detail::TestPoint;
	const std::string inf = root + "\\infer";
	CreateDirectoryA(inf.c_str(), nullptr);
	CreateDirectoryA((inf + "\\sub").c_str(), nullptr);
	Put(inf + "\\sub\\a.vmt");
	long long listed = Listed(), inferred = Inferred();
	Check("a folder that isn't there: one listing", Ask(inf + "\\nofolder\\x.vmt") == Answer::Absent && Listed() == listed + 1);
	Check("below an absent folder: absent, not listed", Ask(inf + "\\nofolder\\deeper\\y.vmt") == Answer::Absent &&
		Listed() == listed + 1 && Inferred() == inferred + 1);
	Check("further below, past a folder not known: absent, not listed",
		Ask(inf + "\\nofolder\\a\\b\\z.vmt") == Answer::Absent && Listed() == listed + 1 && Inferred() == inferred + 2);
	Ask(inf + "\\here.vmt");   // lists infer itself
	listed = Listed();
	inferred = Inferred();
	Check("a name a listed folder doesn't have: absent, not listed", Ask(inf + "\\other\\x.vmt") == Answer::Absent &&
		Listed() == listed && Inferred() == inferred + 1);
	Check("a folder the listing has is listed itself", Ask(inf + "\\SUB\\a.vmt") == Answer::Present && Listed() == listed + 1);
	NewJoin();
	listed = Listed();
	inferred = Inferred();
	Check("next join: not from a folder this join hasn't checked", Ask(inf + "\\other2\\x.vmt") == Answer::Absent &&
		Listed() == listed + 1 && Inferred() == inferred);

	// A folder made after the one above it was listed, then a write into it.
	Ask(inf + "\\here.vmt");   // infer, listed again this join
	inferred = Inferred();
	Check("a folder not made yet: absent from the folder above", Ask(inf + "\\made\\x.vmt") == Answer::Absent &&
		Inferred() == inferred + 1);
	CreateDirectoryA((inf + "\\made").c_str(), nullptr);
	const std::string made = DirOf(inf + "\\made\\x.vmt");
	Invalidate(made);   // what the write's open does before...
	Put(inf + "\\made\\x.vmt");
	Invalidate(made);   // ...and after
	Check("made, then written: present (the stale folder above was dropped too)", Ask(inf + "\\made\\x.vmt") == Answer::Present);
	Check("made, then written: another name in it absent", Ask(inf + "\\made\\y.vmt") == Answer::Absent);

	// A file made during a listing (that took over 2 s) isn't missed next join.
	Clear();
	const std::string during = root + "\\during";
	CreateDirectoryA(during.c_str(), nullptr);
	Put(during + "\\old.vmt");
	SetFolderTime(during, 60);
	SetHook(TestPoint::Listed, during + "\\new.vmt", "", true);
	Check("made during the listing: not in it", Ask(during + "\\new.vmt") == Answer::Absent);
	SetTestHook(nullptr);
	Check("the hook ran", g_hookDone);
	NewJoin();
	before = Reused();
	Check("next join: a listing a file was made during isn't reused", Ask(during + "\\new.vmt") == Answer::Present &&
		Reused() == before);

	// A write while a folder is being listed: what the listing says isn't kept.
	Clear();
	const std::string race = root + "\\race";
	CreateDirectoryA(race.c_str(), nullptr);
	Put(race + "\\a.vmt");
	listed = Listed();
	SetHook(TestPoint::BeforeInsert, race + "\\b.vmt", DirOf(race + "\\b.vmt"), false);
	Check("a write between the listing and keeping it: can't tell", Ask(race + "\\b.vmt") == Answer::Unknown);
	SetTestHook(nullptr);
	Check("a write between the listing and keeping it: nothing kept", Folders() == 0);
	Check("asked again: listed again, the file is there", Ask(race + "\\b.vmt") == Answer::Present && Listed() == listed + 2);
	inferred = Inferred();
	SetHook(TestPoint::BeforeInsert, "", DirOf(race + "\\b.vmt"), false);
	Check("a write while answering from the folder above: can't tell", Ask(race + "\\nofolder\\x.vmt") == Answer::Unknown &&
		Inferred() == inferred + 1);
	SetTestHook(nullptr);
	Check("a write while answering from the folder above: nothing kept", Folders() == 0);

	// A folder that can't be read: one listing per join, not one per open.
	Clear();
	const std::string locked = root + "\\locked";
	CreateDirectoryA(locked.c_str(), nullptr);
	Put(locked + "\\x.vmt");
	bool denied = false;
	if (DenyListing(locked, true))
	{
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileExA((locked + "\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
		denied = h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED;
		if (h != INVALID_HANDLE_VALUE)
			FindClose(h);
	}
	if (denied)
	{
		listed = Listed();
		inferred = Inferred();
		Check("can't be read: can't tell", Ask(locked + "\\x.vmt") == Answer::Unknown && Listed() == listed + 1);
		Check("can't be read: not listed again this join", Ask(locked + "\\y.vmt") == Answer::Unknown && Listed() == listed + 1);
		Ask(locked + "\\deeper\\z.vmt");
		Check("can't be read: nothing below it answered from it", Inferred() == inferred);
		NewJoin();
		listed = Listed();
		Check("next join: listed once again", Ask(locked + "\\x.vmt") == Answer::Unknown && Listed() == listed + 1);
		Check("next join: still once", Ask(locked + "\\y.vmt") == Answer::Unknown && Listed() == listed + 1);
	}
	else
	{
		std::printf("SKIP a folder that can't be read (couldn't deny listing one here)\n");
	}
	DenyListing(locked, false);

	// Listings no join used in the last three are dropped.
	Clear();
	Ask(root + "\\materials\\here.vmt");   // join J
	Ask(root + "\\pruned\\x.vmt");         // join J, absent
	NewJoin();
	Ask(root + "\\pruned\\x.vmt");         // join J + 1
	NewJoin();
	NewJoin();
	Check("used three joins ago: kept", Folders() == 2);
	NewJoin();
	Check("used four joins ago: dropped", Folders() == 1);
	Check("used three joins ago: still answers", Ask(root + "\\pruned\\x.vmt") == Answer::Absent);
	Clear();

	DeleteFileA((inf + "\\sub\\a.vmt").c_str());
	DeleteFileA((inf + "\\made\\x.vmt").c_str());
	RemoveDirectoryA((inf + "\\sub").c_str());
	RemoveDirectoryA((inf + "\\made").c_str());
	RemoveDirectoryA(inf.c_str());
	DeleteFileA((during + "\\old.vmt").c_str());
	DeleteFileA((during + "\\new.vmt").c_str());
	RemoveDirectoryA(during.c_str());
	DeleteFileA((race + "\\a.vmt").c_str());
	DeleteFileA((race + "\\b.vmt").c_str());
	RemoveDirectoryA(race.c_str());
	DeleteFileA((locked + "\\x.vmt").c_str());
	RemoveDirectoryA(locked.c_str());

	// Install after Uninstall: the kept copy goes back over the game's own vtable only.
	int original = 0, ours = 0, foreign = 0;
	void* slot = &original;
	Check("reinstall: the game's vtable is replaced by the kept copy", SwapIn(&slot, &original, &ours) && slot == &ours);
	Check("reinstall: already ours, nothing to do", SwapIn(&slot, &original, &ours) && slot == &ours);
	slot = &foreign;
	Check("reinstall: someone else's vtable is left alone", !SwapIn(&slot, &original, &ours) && slot == &foreign);

	DeleteFileA((root + "\\materials\\Here.VMT").c_str());
	DeleteFileA((root + "\\materials\\gone.vmt").c_str());
	RemoveDirectoryA((root + "\\materials").c_str());
	RemoveDirectoryA(root.c_str());
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
