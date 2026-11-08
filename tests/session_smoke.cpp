// The rejoin-after-a-crash session file, no game:
//   session_smoke <empty scratch folder>
// Exit code 0 when every check passes.

#include "../src/resilience.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

static std::string Read(const std::wstring& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A crash report the game would write: crashes\<name> with a Lua section, written at `when` (unix).
static void CrashFile(const std::wstring& root, const wchar_t* name, long long when)
{
	const std::wstring path = root + L"\\crashes\\" + name;
	{
		std::ofstream f(path, std::ios::binary | std::ios::trunc);
		f << "Crash\r\n-Lua Stack Traces-\r\n==================\r\n  Client\r\n    0. x - addons/foo/lua/x.lua:1\r\n";
	}
	HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	ULARGE_INTEGER t;
	t.QuadPart = static_cast<unsigned long long>(when + 11644473600LL) * 10000000ULL;
	FILETIME ft = { t.LowPart, t.HighPart };
	SetFileTime(h, nullptr, nullptr, &ft);
	CloseHandle(h);
}

// on_server.txt as the menu leaves it: address, the session's start, the name.
static void Session(const std::wstring& file, const std::string& address, long long since)
{
	std::ofstream f(file, std::ios::binary | std::ios::trunc);
	f << address << " " << since << "\r\nMeowy\r\n";
}

// The files this test makes in `dir`: the pack's own and the fake game's crash reports.
static void CleanUp(const std::wstring& dir)
{
	for (const wchar_t* f : { L"on_server.txt", L"last_server.txt", L"meowy_crashes.txt" })
		DeleteFileW((dir + L"\\" + f).c_str());
	const std::wstring crashes = dir + L"\\game\\crashes";
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((crashes + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				DeleteFileW((crashes + L"\\" + fd.cFileName).c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(crashes.c_str());
	RemoveDirectoryW((dir + L"\\game").c_str());
}

int main(int argc, char** argv)
{
	using namespace resilience;
	Check("name kept", CleanName("Foo RP | 24/7") == "Foo RP | 24/7");
	Check("line breaks and controls dropped", CleanName("a\r\nb\tc\x01" "d") == "abcd");
	Check("capped at 64 bytes", CleanName(std::string(100, 'x')).size() == 64);
	// 63 ASCII bytes then a 2-byte character: cut before it, not through it.
	const std::string utf8 = std::string(63, 'x') + "\xc3\xa9" + "yyy";
	const std::string cut = CleanName(utf8);
	Check("a UTF-8 character isn't split", cut.size() == 63 && cut == std::string(63, 'x'));

	wchar_t full[MAX_PATH];
	std::string root = argc > 1 ? argv[1] : ".";
	GetFullPathNameW(std::wstring(root.begin(), root.end()).c_str(), MAX_PATH, full, nullptr);
	const std::wstring dir = full;
	const std::wstring file = dir + L"\\on_server.txt";
	// Everything this test writes, gone before it starts and after it ends: the
	// folder may be shared with another run (CI runs each bitness in one temp folder).
	CleanUp(dir);

	MarkSession(dir, "1.2.3.4:27015", "Foo\r\nRP");
	const std::string text = Read(file);
	Check("file: address, time, then the cleaned name", text.rfind("1.2.3.4:27015 ", 0) == 0 && text.find("\r\nFooRP\r\n") != std::string::npos);
	MarkSession(dir, "1.2.3.4:27015 evil", "x");
	Check("an address with a space isn't written", Read(file) == text);
	std::string name = "unchanged";
	Check("no crash report since: nothing offered", CrashedSession(dir, &name).empty());
	Check("and the file is gone", GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);
	MarkSession(dir, "1.2.3.4:27015", "x");
	ClearSession(dir);
	Check("cleared on leaving", GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);

	// The main menu's Continue: last_server.txt, kept until the next server.
	const std::wstring last = dir + L"\\last_server.txt";
	std::string lastName = "unchanged";
	long long since = -1;
	Check("continue: none kept yet", LastServer(dir, &lastName, &since).empty() && lastName == "unchanged" && since == -1);
	MarkLastServer(dir, "1.2.3.4:27015", "Foo\tRP");
	Check("continue: kept", LastServer(dir, &lastName, &since) == "1.2.3.4:27015");
	Check("continue: cleaned name", lastName == "FooRP");
	Check("continue: its time", since > 1700000000);
	Check("continue: read again, still there", LastServer(dir) == "1.2.3.4:27015");
	ClearSession(dir);
	Check("continue: leaving a server doesn't forget it", LastServer(dir) == "1.2.3.4:27015");
	MarkLastServer(dir, "5.6.7.8:27016", "");
	Check("continue: the next server replaces it", LastServer(dir, &lastName) == "5.6.7.8:27016" && lastName.empty());
	MarkLastServer(dir, "", "x");
	MarkLastServer(dir, "9.9.9.9:1\r\nevil", "x");
	Check("continue: an empty or broken address isn't written", LastServer(dir) == "5.6.7.8:27016");
	{
		std::ofstream junk(last, std::ios::binary | std::ios::trunc);
		junk << "garbage";
	}
	Check("continue: a damaged file offers nothing", LastServer(dir).empty());
	ForgetLastServer(dir);
	Check("continue: forgotten", GetFileAttributesW(last.c_str()) == INVALID_FILE_ATTRIBUTES && LastServer(dir).empty());

	// Crash reports: noted as Meowy Network's only when the session left over
	// was on our server, and only the ones written in it, before this start.
	const std::wstring crashRoot = dir + L"\\game";
	CreateDirectoryW(crashRoot.c_str(), nullptr);
	CreateDirectoryW((crashRoot + L"\\crashes").c_str(), nullptr);
	const long long now = static_cast<long long>(time(nullptr));
	CrashFile(crashRoot, L"before.txt", now - 5000);
	CrashFile(crashRoot, L"ours.txt", now - 100);
	CrashFile(crashRoot, L"later.txt", now + 600);   // written after this start: not that session's
	Session(file, "138.201.62.52:27015", now - 200);
	std::string crashedName;
	Check("crash on ours: offered", CrashedSession(dir, &crashedName, &crashRoot) == "138.201.62.52:27015" && crashedName == "Meowy");
	const std::vector<std::string> asked = { "before.txt", "ours.txt", "later.txt", "other.txt" };
	std::vector<std::string> ours = OurCrashes(dir, asked);
	Check("crash on ours: only that session's report noted", ours.size() == 1 && ours[0] == "ours.txt");
	CrashFile(crashRoot, L"other.txt", now - 50);
	Session(file, "1.2.3.4:27015", now - 80);
	Check("crash on another server: still offered", CrashedSession(dir, nullptr, &crashRoot) == "1.2.3.4:27015");
	ours = OurCrashes(dir, asked);
	Check("crash on another server: never noted as ours", ours.size() == 1 && ours[0] == "ours.txt");
	Session(file, "138.201.62.52:27015", now - 10);
	Check("ours, no report since: nothing offered", CrashedSession(dir, nullptr, &crashRoot).empty());
	Check("ours, no report since: nothing noted", OurCrashes(dir, asked).size() == 1);
	NoteOurCrashes(dir, { "x\\..\\y.txt", "", "ours.txt" });
	Check("noted names: no paths, no repeats", OurCrashes(dir, { "x\\..\\y.txt", "ours.txt" }).size() == 1 && Read(dir + L"\\meowy_crashes.txt") == "ours.txt\r\n");
	std::vector<std::string> many;
	for (int i = 0; i < 60; ++i)
		many.push_back("c" + std::to_string(i) + ".txt");
	NoteOurCrashes(dir, many);
	Check("noted names: the newest 50 kept", OurCrashes(dir, { "ours.txt", "c9.txt", "c10.txt", "c59.txt" }).size() == 2);

	CleanUp(dir);
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
