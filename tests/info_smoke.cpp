// Prints the hardware profile and, given the GarrysMod folder, the newest
// crash reports' fault lines, without the game.
// Usage: info_smoke [path to the GarrysMod folder]
#include "../src/crash_reports.h"
#include "../src/hardware.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int MaskChecks()
{
	struct Case { const char* in; const char* want; };
	const Case cases[] = {
		{ "C:\\Users\\Jane Doe\\gmod\\x.lua:12: attempt", "<path>:12: attempt" },
		{ "open 'D:/Steam Library/steamapps/common/GarrysMod/garrysmod/data/a.txt' failed", "open '<path>' failed" },
		{ "C:\\Program Files (x86)\\Steam\\x.lua: bad argument", "<path>: bad argument" },
		{ "\\\\nas\\share\\Jane\\x.lua:3:", "<path>:3:" },
		{ "line one C:\\a b\\c\nline two", "line one <path>\nline two" },
		{ "addons/foo/lua/x.lua:12: attempt to index nil", "addons/foo/lua/x.lua:12: attempt to index nil" },
		{ "time 12:30 abc:\\x", "time 12:30 abc:\\x" },
	};
	int failed = 0;
	for (const Case& c : cases)
	{
		std::string got = crashes::MaskPaths(c.in);
		bool ok = got == c.want;
		std::printf("%s mask: %s -> %s\n", ok ? "ok  " : "FAIL", c.in, got.c_str());
		failed += ok ? 0 : 1;
	}
	return failed;
}

// A crash report the game would write, in root\crashes, dated `when` (unix).
static void CrashFile(const std::wstring& root, const wchar_t* name, long long when, bool lua)
{
	const std::wstring path = root + L"\\crashes\\" + name;
	{
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		f << "Crash\r\n";
		if (lua)
			f << "-Lua Stack Traces-\r\n==================\r\n  Client\r\n    0. x - addons/foo/lua/x.lua:1\r\n";
	}
	HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	ULARGE_INTEGER t;
	t.QuadPart = static_cast<unsigned long long>(when + 11644473600LL) * 10000000ULL;
	FILETIME ft = { t.LowPart, t.HighPart };
	SetFileTime(h, nullptr, nullptr, &ft);
	CloseHandle(h);
}

// The listing-first reads (crashes::Candidates and Read) in a throwaway folder
// under %TEMP%: listed by time without opening anything, read one at a time,
// a report without a Lua section never returned.
static int CandidateChecks()
{
	int failed = 0;
	auto check = [&failed](const char* name, bool ok) {
		std::printf("%s crashes: %s\n", ok ? "ok  " : "FAIL", name);
		failed += ok ? 0 : 1;
	};

	wchar_t temp[MAX_PATH];
	const DWORD n = GetTempPathW(MAX_PATH, temp);
	if (n == 0 || n >= MAX_PATH)
		return 1;
	const std::wstring root = std::wstring(temp) + L"info_smoke_" + std::to_wstring(GetCurrentProcessId());
	CreateDirectoryW(root.c_str(), nullptr);
	CreateDirectoryW((root + L"\\crashes").c_str(), nullptr);
	const wchar_t* files[] = { L"a.txt", L"b.txt", L"c.txt", L"d.txt" };
	CrashFile(root, L"a.txt", 1000000, true);
	CrashFile(root, L"b.txt", 3000000, false);   // the newest, but no Lua section
	CrashFile(root, L"c.txt", 2000000, true);
	CrashFile(root, L"d.txt", 500000, true);     // older than `since`

	const std::vector<crashes::Candidate> listed = crashes::Candidates(root, 600000);
	bool names = listed.size() == 3;
	for (const crashes::Candidate& c : listed)
		names = names && c.name != "d.txt" && c.time > 600000;
	check("listed: only those after since", names);

	const std::vector<crashes::Report> one = crashes::SinceIn(root, 600000, 1);
	check("newest with a trace first", one.size() == 1 && one[0].name == "c.txt" && one[0].time == 2000000);
	const std::vector<crashes::Report> all = crashes::SinceIn(root, 600000, 5);
	check("no trace: skipped", all.size() == 2 && all[0].name == "c.txt" && all[1].name == "a.txt");

	bool readA = false, readB = true;
	for (const crashes::Candidate& c : listed)
	{
		crashes::Report r;
		if (c.name == "a.txt")
			readA = crashes::Read(root, c, r) && r.name == "a.txt" && r.time == 1000000 && !r.luaTrace.empty() && r.fault.empty();
		else if (c.name == "b.txt")
			readB = crashes::Read(root, c, r);
	}
	check("read one: trace, no dump", readA);
	check("read one without a trace: false", !readB);
	check("no folder: nothing", crashes::Candidates(L"", 0).empty() && crashes::SinceIn(root + L"\\none", 0, 5).empty());

	for (const wchar_t* f : files)
		DeleteFileW((root + L"\\crashes\\" + f).c_str());
	RemoveDirectoryW((root + L"\\crashes").c_str());
	RemoveDirectoryW(root.c_str());
	return failed;
}

int main(int argc, char** argv)
{
	if (int failed = MaskChecks())
		return failed;
	if (int failed = CandidateChecks())
		return failed;
	hardware::Info hw = hardware::Get();
	std::printf("cpu: %s\nthreads: %u\nram: %llu MB\ngpu: %s\nvram: %llu MB\nos: %s\n",
		hw.cpu.c_str(), hw.threads, hw.ramMb, hw.gpu.c_str(), hw.vramMb, hw.os.c_str());

	if (argc > 1)
	{
		std::string a(argv[1]);
		std::wstring root(a.begin(), a.end());
		for (const crashes::Report& r : crashes::SinceIn(root, 0, 6))
			std::printf("crash %lld: fault=%s  lua=%zu bytes\n", r.time, r.fault.c_str(), r.luaTrace.size());
	}
	return 0;
}
