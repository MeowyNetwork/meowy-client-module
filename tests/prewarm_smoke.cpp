// Join pre-warm, no game:
//   prewarm_smoke <empty scratch folder>
// The per-server list format, recording during a join, keeping a list per
// server, and a warm run over scratch files (on this machine's drive).
// Exit code 0 when every check passes.

#include "../src/prewarm.h"
#include "../src/file_util.h"

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

// EndJoin hands the list's merge and write to the I/O thread: the checks that
// read the list back wait for it.
static int EndJoinAndWait(const std::wstring& dir, const std::string& address)
{
	const int noted = prewarm::EndJoin(dir, address);
	if (!prewarm::detail::WaitIoIdle(10000))
		std::printf("  (the I/O thread took over 10 s)\n");
	return noted;
}

static bool Exists(const std::wstring& path)
{
	return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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
	Check("kept for a public address", EndJoinAndWait(dir, "1.2.3.4:27015") == 2);
	uint64_t bytes = 0;
	Check("recorded, with its size", prewarm::Recorded(dir, "1.2.3.4:27015", &bytes) && bytes == (52ull << 20));
	Check("not recorded for another server", !prewarm::Recorded(dir, "5.6.7.8:27015"));
	Check("ending again keeps nothing", EndJoinAndWait(dir, "1.2.3.4:27015") == 0);
	Check("a join with nothing big keeps nothing", (prewarm::BeginJoin(), EndJoinAndWait(dir, "9.9.9.9:1")) == 0 && !prewarm::Recorded(dir, "9.9.9.9:1"));

	// At most 60 servers kept: the oldest go.
	for (int i = 0; i < 65; ++i)
	{
		prewarm::BeginJoin();
		prewarm::Opened("c:\\x.gma", 2 << 20);
		EndJoinAndWait(dir, "10.0.0." + std::to_string(i) + ":1");
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
	Check("workshop: kept with the files", EndJoinAndWait(dir, "7.7.7.7:27015") == 2);
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
	EndJoinAndWait(dir, "2.2.2.2:27015");
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
	EndJoinAndWait(dir, "4.4.4.4:27015");
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

	// Ranges: the blocks a join reads of files not on the list whole (the base
	// game's archives, small loose files), kept as "range" lines and warmed.
	{
		using prewarm::detail::Range;
		const std::vector<Range> one = { { "c:\\v.vpk", 10 << 20, { { 0, 2 }, { 5, 6 } } } };
		const std::string text = prewarm::detail::Format({}, {}, one);
		Check("ranges: written as a range line", text == "range\tc:\\v.vpk\t10485760\t0-2,5-6\r\n");
		std::vector<prewarm::detail::Item> noItems;
		std::vector<Range> parsed;
		Check("ranges: read back", prewarm::detail::Parse(text, noItems, nullptr, &parsed) && parsed.size() == 1 &&
			parsed[0].path == "c:\\v.vpk" && parsed[0].runs.size() == 2 && parsed[0].runs[1].first == 5 && parsed[0].runs[1].second == 6);
		parsed.clear();
		prewarm::detail::Parse("range\tc:\\x\t100\t1-0\r\nrange\tc:\\y\t1048576\t0-5\r\nrange\tc:\\z\t0\t0-1\r\nrange\tc:\\w\t5\t\r\n", noItems, nullptr, &parsed);
		Check("ranges: odd lines left out", parsed.empty());

		const std::string vpk = root + "\\base_000.vpk", small = root + "\\tiny.vtf";
		Put(vpk, 5 << 20);
		Put(small, 1000);
		prewarm::BeginJoin();
		prewarm::detail::NoteRead(vpk, 5 << 20, 0, 100);
		prewarm::detail::NoteRead(vpk, 5 << 20, (7 << 20) / 2, 1 << 20);   // 3.5 MB on: blocks 3 and 4
		prewarm::detail::NoteRead(small, 1000, 0, 1000);
		prewarm::detail::NoteRead(small, 1000, 5000, 10);                   // past its end: nothing
		Check("ranges: kept with the join", EndJoinAndWait(dir, "6.6.6.6:27015") == 2);
		uint64_t rb = 0;
		Check("ranges: their size counts, only the blocks read", prewarm::Recorded(dir, "6.6.6.6:27015", &rb) && rb == (3ull << 20) + 1000);
		prewarm::BeginJoin();
		prewarm::detail::NoteRead(vpk, 5 << 20, 1 << 20, 10);
		EndJoinAndWait(dir, "6.6.6.6:27015");
		std::vector<Range> list;
		prewarm::detail::Parse(file_util::ReadSmall(prewarm::detail::FileFor(dir, "6.6.6.6:27015"), 4 << 20), noItems, nullptr, &list);
		const Range* v = nullptr;
		for (const Range& r : list)
			if (r.path == vpk)
				v = &r;
		Check("ranges: a later join's blocks join the earlier ones", v && v->runs.size() == 2 && v->runs[0].first == 0 && v->runs[0].second == 2 &&
			v->runs[1].first == 3 && v->runs[1].second == 5);
		Check("ranges: a file the later join didn't read stays", list.size() == 2);
		prewarm::BeginJoin();
		prewarm::Opened(vpk.c_str(), 5 << 20);
		prewarm::detail::NoteRead(vpk, 5 << 20, 0, 10);
		EndJoinAndWait(dir, "6.6.6.6:27015");
		list.clear();
		std::vector<prewarm::detail::Item> wholes;
		prewarm::detail::Parse(file_util::ReadSmall(prewarm::detail::FileFor(dir, "6.6.6.6:27015"), 4 << 20), wholes, nullptr, &list);
		Check("ranges: a file on the list whole has no range", wholes.size() == 1 && list.size() == 1 && list[0].path == small);
		std::vector<Range> now = { { "c:\\q.vpk", 4 << 20, { { 0, 1 } } } }, earlier = { { "C:\\Q.VPK", 8 << 20, { { 5, 6 } } } };
		prewarm::detail::MergeRanges(now, earlier, {}, nullptr);
		Check("ranges: a file that changed size keeps this join's blocks", now.size() == 1 && now[0].size == (4 << 20) && now[0].runs.size() == 1);

		// Warmed: the ranges' blocks.
		const int ssdHere = NoSeekPenalty(std::wstring(root.begin(), root.end()));
		prewarm::BeginJoin();
		prewarm::detail::NoteRead(vpk, 5 << 20, 0, 100);
		prewarm::detail::NoteRead(small, 1000, 0, 1000);
		EndJoinAndWait(dir, "6.6.6.7:27015");
		std::string whyRanges;
		const bool startedRanges = prewarm::Start(dir, "6.6.6.7:27015", 1ull << 30, whyRanges);
		if (ssdHere == 1)
		{
			Check("ranges: warming starts", startedRanges);
			for (int i = 0; i < 300 && prewarm::Describe().find("warmed 6.6.6.7") == std::string::npos; ++i)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			Check("ranges: only their blocks read", prewarm::Describe().find("warmed 6.6.6.7:27015: 1 MB in 2 files") != std::string::npos);
		}
		DeleteFileA(vpk.c_str());
		DeleteFileA(small.c_str());
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
		EndJoinAndWait(dir, "3.3.3.3:27015");
		DeleteFileA(k3.c_str());
		prewarm::BeginJoin();
		prewarm::Opened(k1.c_str(), 2 << 20);
		// EndJoin answers with what this join noted; the merge comes after.
		Check("rejoin: this join's file noted", EndJoinAndWait(dir, "3.3.3.3:27015") == 1);
		uint64_t keptBytes = 0;
		Check("rejoin: the first join's files kept, a file gone since dropped",
			prewarm::Recorded(dir, "3.3.3.3:27015", &keptBytes) && keptBytes == (5ull << 20));
		DeleteFileA(k1.c_str());
		DeleteFileA(k2.c_str());
	}

	// EndJoin returns at once; the list is written on the I/O thread just after.
	// The thread is held busy first, so the order is certain.
	{
		const std::string a1 = root + "\\async1.gma";
		Put(a1, 2 << 20);
		HANDLE hold = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		prewarm::detail::RunOnIoThread([hold]() { WaitForSingleObject(hold, 10000); });
		const std::wstring file = FileFor(dir, "5.5.5.5:27015");
		prewarm::BeginJoin();
		prewarm::Opened(a1.c_str(), 2 << 20);
		const auto t0 = std::chrono::steady_clock::now();
		const int noted = prewarm::EndJoin(dir, "5.5.5.5:27015");
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("  EndJoin returned in %.2f ms\n", ms);
		Check("async: EndJoin returns what this join noted", noted == 1);
		Check("async: not written while the I/O thread is busy", !Exists(file));
		SetEvent(hold);
		bool written = false;
		for (int i = 0; i < 500 && !(written = Exists(file)); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		Check("async: written shortly after", written);
		prewarm::detail::WaitIoIdle(10000);
		CloseHandle(hold);

		// Two quick joins of one server: the second merges with the first's
		// list, written before it (one job after another), so both are kept.
		const std::string a2 = root + "\\async2.gma";
		Put(a2, 3 << 20);
		prewarm::BeginJoin();
		prewarm::Opened(a1.c_str(), 2 << 20);
		prewarm::EndJoin(dir, "5.5.5.6:27015");
		prewarm::BeginJoin();
		prewarm::Opened(a2.c_str(), 3 << 20);
		prewarm::EndJoin(dir, "5.5.5.6:27015");
		prewarm::detail::WaitIoIdle(10000);
		uint64_t both = 0;
		Check("two quick joins: both kept", prewarm::Recorded(dir, "5.5.5.6:27015", &both) && both == (5ull << 20));

		// The menu's questions share one parse until the list is written again.
		const uint64_t parses = ListParses();
		uint64_t b1 = 0, b2 = 0;
		prewarm::Recorded(dir, "5.5.5.6:27015", &b1);
		prewarm::WorkshopItems(dir, "5.5.5.6:27015");
		prewarm::Recorded(dir, "5.5.5.6:27015", &b2);
		Check("parse cache: asked three times, read once at most", ListParses() - parses <= 1 && b1 == b2 && b1 == (5ull << 20));
		const uint64_t cached = ListParses();
		prewarm::Recorded(dir, "5.5.5.6:27015");
		Check("parse cache: read again only when it changes", ListParses() == cached);
		const std::string a3 = root + "\\async3.gma";
		Put(a3, 4 << 20);
		prewarm::BeginJoin();
		prewarm::Opened(a3.c_str(), 4 << 20);
		EndJoinAndWait(dir, "5.5.5.6:27015");
		uint64_t after = 0;
		Check("parse cache: a new write is read again", prewarm::Recorded(dir, "5.5.5.6:27015", &after) &&
			after == (9ull << 20) && ListParses() == cached + 1);
		DeleteFileW(FileFor(dir, "5.5.5.6:27015").c_str());
		Check("parse cache: a deleted list is gone", !prewarm::Recorded(dir, "5.5.5.6:27015") &&
			prewarm::WorkshopItems(dir, "5.5.5.6:27015").empty());
		DeleteFileA(a1.c_str());
		DeleteFileA(a2.c_str());
		DeleteFileA(a3.c_str());
	}

	// The read hook's file look-up: the volume's NT name mapped to its drive
	// letter, and the slower name when the table hasn't got that volume.
	{
		const DeviceMap fake = { { L"\\Device\\HarddiskVolume3", L"C:" }, { L"\\Device\\HarddiskVolume12", L"D:" } };
		Check("devices: a volume to its letter", DosPath(L"\\Device\\HarddiskVolume3\\Games\\x.vpk", fake) == L"C:\\Games\\x.vpk");
		Check("devices: any case", DosPath(L"\\device\\harddiskvolume12\\a", fake) == L"D:\\a");
		Check("devices: a longer volume number isn't a match", DosPath(L"\\Device\\HarddiskVolume30\\x", fake).empty());
		Check("devices: nothing for a volume not in the table", DosPath(L"\\Device\\Mup\\server\\share\\x", fake).empty());
		Check("devices: nothing for the volume alone", DosPath(L"\\Device\\HarddiskVolume3", fake).empty());
		const DeviceMap mine = DevicePrefixes();
		bool hasRootDrive = false;
		for (const auto& d : mine)
			hasRootDrive = hasRootDrive || (d.second.size() == 2 && (d.second[0] | 0x20) == (root[0] | 0x20));
		Check("devices: this PC's table has the scratch folder's drive", hasRootDrive);

		const std::string probe = root + "\\Probe File.vpk";
		Put(probe, 3000);
		HANDLE f = CreateFileA(probe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		wchar_t dos[1024];
		const DWORD n = GetFinalPathNameByHandleW(f, dos, 1024, FILE_NAME_OPENED | VOLUME_NAME_DOS);
		std::wstring expectedW(dos, n);
		if (expectedW.compare(0, 4, L"\\\\?\\") == 0)
			expectedW = expectedW.substr(4);
		std::string expected;
		for (wchar_t c : expectedW)
			expected.push_back(static_cast<char>(c));   // the scratch paths here are ASCII
		std::string viaTable, viaFallback;
		int64_t size1 = 0, size2 = 0;
		UseDevices(mine);
		const bool ok1 = FileOfHandle(f, viaTable, size1);
		UseDevices({});
		const bool ok2 = FileOfHandle(f, viaFallback, size2);
		UseDevices(mine);
		std::printf("  %s\n", viaTable.c_str());
		Check("handle: found through the table", ok1 && viaTable == expected && size1 == 3000);
		Check("handle: the same through the slower name", ok2 && viaFallback == expected && size2 == 3000);
		CloseHandle(f);
		DeleteFileA(probe.c_str());

		// The game's data folder isn't noted.
		const std::string dataDir = root + "\\garrysmod";
		CreateDirectoryA(dataDir.c_str(), nullptr);
		CreateDirectoryA((dataDir + "\\data").c_str(), nullptr);
		const std::string dataFile = dataDir + "\\data\\x.txt";
		Put(dataFile, 100);
		f = CreateFileA(dataFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		std::string dataPath;
		int64_t dataSize = 0;
		Check("handle: not a file under garrysmod\\data", !FileOfHandle(f, dataPath, dataSize));
		CloseHandle(f);
		DeleteFileA(dataFile.c_str());
		RemoveDirectoryA((dataDir + "\\data").c_str());
		RemoveDirectoryA(dataDir.c_str());
	}

	// Each read is counted for the status.
	{
		prewarm::BeginJoin();
		prewarm::detail::NoteRead("c:\\count.vpk", 4 << 20, 0, 10);
		prewarm::detail::NoteRead("c:\\count.vpk", 4 << 20, 1 << 20, 10);
		prewarm::detail::NoteRead("C:\\COUNT.VPK", 4 << 20, 2 << 20, 10);
		const std::string d = prewarm::Describe();
		Check("status: reads counted", d.find("reads from 1 files") != std::string::npos && d.find("; 3 reads noted") != std::string::npos);
		EndJoinAndWait(dir, std::string());
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
