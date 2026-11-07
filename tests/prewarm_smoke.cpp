// Join pre-warm, no game:
//   prewarm_smoke <empty scratch folder>
// The per-server list format, recording during a join, keeping a list per
// server, and a warm run over scratch files (on this machine's drive).
// Exit code 0 when every check passes.

#include "../src/prewarm.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
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

static void Put(const std::string& path, size_t bytes)
{
	std::ofstream f(path, std::ios::binary);
	std::vector<char> b(bytes, 'x');
	f.write(b.data(), static_cast<std::streamsize>(b.size()));
}

int main(int argc, char** argv)
{
	using namespace prewarm::detail;
	char full[MAX_PATH];
	GetFullPathNameA(argc > 1 ? argv[1] : ".", MAX_PATH, full, nullptr);
	const std::string root = full;
	const std::wstring dir(root.begin(), root.end());

	// Names
	Check("file for an address", FileFor(L"C:\\x", "1.2.3.4:27015") == L"C:\\x\\joins\\1.2.3.4_27015.txt");
	Check("nothing for a name with other characters", FileFor(L"C:\\x", "evil/../x:1").empty());
	Check("nothing for an empty address", FileFor(L"C:\\x", "").empty());

	// The list format
	std::vector<Item> items;
	Check("parse", Parse("c:\\a\\b.gma\t2097152\r\nc:\\maps\\m.bsp\t1048576\r\n", items) && items.size() == 2);
	Check("parse: path and size", items.size() == 2 && items[0].path == "c:\\a\\b.gma" && items[0].size == 2097152);
	items.clear();
	Check("parse: small or malformed lines skipped", !Parse("c:\\small\t100\r\nno tab\r\n\tsize only 5000000\r\nc:\\x\tbig\r\n", items));
	const std::vector<Item> two = { { "c:\\a b\\c.gma", 5000000 }, { "c:\\d\te.gma", 5000000 } };
	std::vector<Item> back;
	Parse(Format(two), back);
	Check("format: round trip, and a path with a tab left out", back.size() == 1 && back[0].path == "c:\\a b\\c.gma");

	// Recording: only during a join, 1 MB or more, each file once.
	prewarm::Opened("c:\\before.gma", 5 << 20);
	prewarm::BeginJoin();
	prewarm::Opened("c:\\a.gma", 2 << 20);
	prewarm::Opened("c:\\a.gma", 2 << 20);
	prewarm::Opened("c:\\small.vmt", 1000);
	prewarm::Opened("c:\\maps\\m.bsp", 50 << 20);
	Check("recording shows in the status", prewarm::Describe().find("recording this join: 2 files") != std::string::npos);
	Check("kept for a public address", prewarm::EndJoin(dir, "1.2.3.4:27015") == 2);
	uint64_t bytes = 0;
	Check("recorded, with its size", prewarm::Recorded(dir, "1.2.3.4:27015", &bytes) && bytes == (52ull << 20));
	Check("not recorded for another server", !prewarm::Recorded(dir, "5.6.7.8:27015"));
	Check("ending again keeps nothing", prewarm::EndJoin(dir, "1.2.3.4:27015") == 0);
	Check("a join with nothing big keeps nothing", (prewarm::BeginJoin(), prewarm::EndJoin(dir, "9.9.9.9:1")) == 0 && !prewarm::Recorded(dir, "9.9.9.9:1"));

	// At most 60 servers kept: the oldest go.
	for (int i = 0; i < 65; ++i)
	{
		prewarm::BeginJoin();
		prewarm::Opened("c:\\x.gma", 2 << 20);
		prewarm::EndJoin(dir, "10.0.0." + std::to_string(i) + ":1");
		Sleep(2);
	}
	int kept = 0;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\joins\\*.txt").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do ++kept; while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	Check("at most 60 lists kept", kept == 60);
	Check("the newest kept", prewarm::Recorded(dir, "10.0.0.64:1"));
	Check("the oldest dropped", !prewarm::Recorded(dir, "10.0.0.0:1"));

	// Workshop items: the item behind a path, in Steam's content folder or the game's cache.
	Check("workshop id: Steam's content folder",
		WorkshopId("C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\4000\\3791005669\\meowy_models.gma") == 3791005669ull);
	Check("workshop id: forward slashes, any case", WorkshopId("d:/SteamLibrary/steamapps/Workshop/Content/4000/248302805/out.gma") == 248302805ull);
	Check("workshop id: a legacy item's .bin", WorkshopId("c:\\s\\workshop\\content\\4000\\104479467\\x_legacy.bin") == 104479467ull);
	Check("workshop id: the game's cache", WorkshopId("C:\\gm\\garrysmod\\cache\\workshop\\1311178246.gma") == 1311178246ull);
	Check("workshop id: not another app's items", WorkshopId("c:\\s\\workshop\\content\\440\\123\\x.gma") == 0);
	Check("workshop id: not a cache file that isn't a .gma", WorkshopId("c:\\gm\\cache\\workshop\\123.cache") == 0);
	Check("workshop id: not a folder name without digits", WorkshopId("c:\\s\\workshop\\content\\4000\\abc\\x.gma") == 0);
	Check("workshop id: not an ordinary file", WorkshopId("c:\\gm\\garrysmod\\maps\\rp_x.bsp") == 0);
	// Recorded at any size, once each, and kept with the files.
	const std::string ws1 = "c:\\s\\workshop\\content\\4000\\111\\small.gma";
	const std::string ws2 = "c:\\gm\\cache\\workshop\\222.gma";
	prewarm::Opened(ws1.c_str(), 1000);   // not recording yet
	prewarm::BeginJoin();
	prewarm::Opened(ws2.c_str(), 5 << 20);
	prewarm::Opened(ws1.c_str(), 1000);
	prewarm::Opened(ws1.c_str(), -1);
	prewarm::Opened("c:\\maps\\m.bsp", 50 << 20);
	Check("workshop: recording shows in the status", prewarm::Describe().find("2 files, 2 Workshop items") != std::string::npos);
	Check("workshop: kept with the files", prewarm::EndJoin(dir, "7.7.7.7:27015") == 2);
	const std::vector<uint64_t> wsItems = prewarm::WorkshopItems(dir, "7.7.7.7:27015");
	Check("workshop: both items, in the order opened", wsItems.size() == 2 && wsItems[0] == 222 && wsItems[1] == 111);
	bytes = 0;
	Check("workshop: the file list's size leaves them out", prewarm::Recorded(dir, "7.7.7.7:27015", &bytes) && bytes == (55ull << 20));
	Check("workshop: none for another server", prewarm::WorkshopItems(dir, "8.8.8.8:27015").empty());
	std::vector<uint64_t> wsBack;
	items.clear();
	Check("workshop: parse keeps files and items apart",
		Parse(Format({ { "c:\\a.gma", 2 << 20 } }, { 5, 6 }) + "workshop\tx\r\nworkshop\t\r\n", items, &wsBack)
		&& items.size() == 1 && wsBack.size() == 2 && wsBack[1] == 6);

	// A warm run over real files on this drive.
	const std::string f1 = root + "\\w1.gma", f2 = root + "\\w2.gma", f3 = root + "\\gone.gma";
	Put(f1, 3 << 20);
	Put(f2, 2 << 20);
	prewarm::BeginJoin();
	prewarm::Opened(f1.c_str(), 3 << 20);
	prewarm::Opened(f2.c_str(), 5 << 20);   // recorded bigger than it is now: skipped
	prewarm::Opened(f3.c_str(), 2 << 20);   // gone: skipped
	prewarm::EndJoin(dir, "2.2.2.2:27015");
	const int ssd = NoSeekPenalty(std::wstring(root.begin(), root.end()));
	std::printf("  scratch drive: %s\n", ssd == 1 ? "SSD" : ssd == 0 ? "hard disk" : "unknown");
	std::string why;
	const bool started = prewarm::Start(dir, "2.2.2.2:27015", 1ull << 30, why);
	if (ssd == 1)
	{
		Check("warm starts on an SSD", started);
		for (int i = 0; i < 200 && prewarm::Describe().find("warmed") == std::string::npos; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		const std::string d = prewarm::Describe();
		std::printf("  %s\n", d.c_str());
		Check("warm finished: 3 MB in 1 file (changed and gone files skipped)", d.find("warmed 2.2.2.2:27015: 3 MB in 1 files") != std::string::npos);
	}
	else
	{
		Check("warm refused off an SSD", !started && why.find("SSD") != std::string::npos);
	}
	Check("nothing recorded: refused", !prewarm::Start(dir, "3.3.3.3:1", 1ull << 30, why) && why.find("nothing recorded") != std::string::npos);

	// Cancel stops a run at its next read.
	std::vector<std::string> bigs;
	prewarm::BeginJoin();
	for (int i = 0; i < 20; ++i)
	{
		bigs.push_back(root + "\\big" + std::to_string(i) + ".gma");
		Put(bigs.back(), 8 << 20);
		prewarm::Opened(bigs.back().c_str(), 8 << 20);
	}
	prewarm::EndJoin(dir, "4.4.4.4:27015");
	if (ssd == 1 && prewarm::Start(dir, "4.4.4.4:27015", 1ull << 30, why))
	{
		prewarm::Cancel();
		Check("cancel: not warming any more", prewarm::Describe().find("warming 4.4.4.4") == std::string::npos);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}
	// A budget stops it early.
	if (ssd == 1 && prewarm::Start(dir, "4.4.4.4:27015", 16 << 20, why))
	{
		for (int i = 0; i < 300 && prewarm::Describe().find("warmed 4.4.4.4") == std::string::npos; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		Check("budget: stops at 16 MB", prewarm::Describe().find("warmed 4.4.4.4:27015: 16 MB") != std::string::npos);
	}

	// A rejoin in the same game start opens only what isn't loaded already: what
	// the first join learned stays, while it's on disk at its size.
	{
		std::vector<prewarm::detail::Item> now = { { "C:\\A.gma", 2 << 20 } }, before = { { "c:\\a.gma", 2 << 20 }, { "c:\\b.gma", 3 << 20 } };
		std::vector<uint64_t> ws = { 5 }, wsBefore = { 5, 6 };
		prewarm::detail::Merge(now, ws, before, wsBefore, nullptr);
		Check("merge: this join first, then what it didn't open (any case)", now.size() == 2 && now[0].path == "C:\\A.gma" && now[1].path == "c:\\b.gma");
		Check("merge: Workshop items of both, once", ws.size() == 2 && ws[1] == 6);
		const std::string k1 = root + "\\keep1.gma", k2 = root + "\\keep2.gma", k3 = root + "\\gone2.gma";
		Put(k1, 2 << 20);
		Put(k2, 3 << 20);
		Put(k3, 2 << 20);
		prewarm::BeginJoin();
		prewarm::Opened(k1.c_str(), 2 << 20);
		prewarm::Opened(k2.c_str(), 3 << 20);
		prewarm::Opened(k3.c_str(), 2 << 20);
		prewarm::EndJoin(dir, "3.3.3.3:27015");
		DeleteFileA(k3.c_str());
		prewarm::BeginJoin();
		prewarm::Opened(k1.c_str(), 2 << 20);
		Check("rejoin: the first join's files kept", prewarm::EndJoin(dir, "3.3.3.3:27015") == 2);
		uint64_t keptBytes = 0;
		Check("rejoin: a file gone since is dropped", prewarm::Recorded(dir, "3.3.3.3:27015", &keptBytes) && keptBytes == (5ull << 20));
		DeleteFileA(k1.c_str());
		DeleteFileA(k2.c_str());
	}

	for (const std::string& b : bigs)
		DeleteFileA(b.c_str());
	DeleteFileA(f1.c_str());
	DeleteFileA(f2.c_str());
	h = FindFirstFileW((dir + L"\\joins\\*.txt").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do DeleteFileW((dir + L"\\joins\\" + fd.cFileName).c_str()); while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW((dir + L"\\joins").c_str());
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
