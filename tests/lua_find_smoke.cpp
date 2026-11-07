// The fast file.Find on "LUA", no game running:
//   lua_find_smoke [GarrysMod folder] [file to write the generated stubs to]
// 1. Next() against a fake string table: walking the candidates gives exactly
//    the entries the game's own loop could match, every one the game's
//    wildcard matcher accepts among them, in the game's order.
// 2. The loop is found once in the installed client.dll of this build's
//    bitness, and both stubs can be built for it (written out for the offline
//    disassembly check).
// Exit code 0 when every check passes.

#include "../src/lua_find.h"
#include "../src/pe_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

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

// Same virtual layout as the public Source SDK INetworkStringTable (slots
// 0..9), and the same class name as the engine's, so its RTTI matches.
class CNetworkStringTable
{
public:
	std::vector<std::string> entries;
	virtual ~CNetworkStringTable() {}
	virtual const char* GetTableName() const { return "client_lua_files"; }
	virtual int GetTableId() const { return 7; }
	virtual int GetNumStrings() const { return static_cast<int>(entries.size()); }
	virtual int GetMaxStrings() const { return 65536; }
	virtual int GetEntryBits() const { return 16; }
	virtual void SetTick(int) {}
	virtual bool ChangedSinceTick(int) const { return false; }
	virtual int AddString(bool, const char* value, int, const void*) { entries.push_back(value); return static_cast<int>(entries.size()) - 1; }
	virtual const char* GetString(int i) { return i >= 0 && i < static_cast<int>(entries.size()) ? entries[i].c_str() : nullptr; }
};

class CSomethingElse : public CNetworkStringTable
{
public:
	const char* GetTableName() const override { return "other"; }
};

// The game's matcher (client.dll, both bitnesses), byte for byte in behaviour:
// '*' any run, '?' any one byte, everything else equal; case-sensitive.
static bool GameMatch(const char* pattern, const char* s)
{
	const char* star = nullptr;
	const char* resume = nullptr;
	while (*s)
	{
		if (*pattern == '*')
		{
			star = ++pattern;
			if (!*star)
				return true;
			resume = s;
			continue;
		}
		if (*pattern == *s || *pattern == '?')
		{
			++pattern;
			++s;
			continue;
		}
		if (!star)
			return false;
		pattern = star;
		s = ++resume;
	}
	while (*pattern == '*')
		++pattern;
	return *pattern == 0;
}

static int Index(int64_t r) { return static_cast<int32_t>(static_cast<uint32_t>(r)); }
static int Count(int64_t r) { return static_cast<int32_t>(static_cast<uint64_t>(r) >> 32); }

// Every index the patched loop visits, starting as the game does.
static std::vector<int> Walk(CNetworkStringTable& t, const char* pattern)
{
	using namespace lua_find::detail;
	std::vector<int> out;
	int64_t r = Next(&t, pattern, -1);
	while (r != kLeaveToGame && Index(r) >= 0)
	{
		out.push_back(Index(r));
		r = Next(&t, pattern, Index(r));
	}
	return out;
}

static void Logic()
{
	using namespace lua_find::detail;
	uintptr_t base = 0;
	size_t size = 0;
	pe_image::ImageRange(GetModuleHandleW(nullptr), base, size);
	SetEngineRangeForTest(base, size);
	ResetForTest();

	Check("literal: up to '*'", LiteralPart("lua/autorun/*.lua") == "lua/autorun/");
	Check("literal: up to '?'", LiteralPart("lua/a?c.lua") == "lua/a");
	Check("literal: none", LiteralPart("*") == "");
	Check("literal: all", LiteralPart("lua/x.lua") == "lua/x.lua");
	Check("literal: null", LiteralPart(nullptr) == "");

	CNetworkStringTable t;
	t.entries = { "paths", "lua/autorun/b.lua", "addons/x/lua/autorun/a.lua", "lua/autorun/a.lua",
		"lua/autorun/client/c.lua", "lua/Autorun/upper.lua", "lua/autorunner.lua", "gamemodes/darkrp/gamemode/cl_init.lua",
		"lua/autorun/a.lua.bak", "lua/weapons/m9k_ak47.lua", "lua/autorun", "lua/autoru" };

	std::vector<int> w = Walk(t, "lua/autorun/*");
	Check("walk: only entries with the literal part", w == std::vector<int>({ 1, 3, 4, 8 }));
	Check("walk: in the game's order (by index)", std::is_sorted(w.begin(), w.end()));
	Check("walk: case-sensitive, like the matcher", std::find(w.begin(), w.end(), 5) == w.end());
	Check("walk: an entry shorter than the literal part is skipped", std::find(w.begin(), w.end(), 11) == w.end());
	Check("count in the high bits", Count(Next(&t, "lua/autorun/*", -1)) == 12);
	Check("none left: -1 with the count", Index(Next(&t, "lua/autorun/*", 8)) == -1 && Count(Next(&t, "lua/autorun/*", 8)) == 12);
	Check("a pattern starting with '*' walks everything", Walk(t, "*").size() == t.entries.size());
	Check("a '?' ends the literal part", Walk(t, "lua/auto?un/*") == std::vector<int>({ 1, 3, 4, 6, 8, 10, 11 }));
	Check("no match at all", Walk(t, "lua/nothing/*").empty());
	Check("an exact name", Walk(t, "lua/autorun/a.lua") == std::vector<int>({ 3, 8 }));

	// Against the game's matcher: every entry it accepts is visited, and every
	// skipped entry is one it rejects.
	const char* patterns[] = { "lua/autorun/*", "lua/autorun/*.lua", "lua/autorun/client/*", "*", "lua/*/a.lua",
		"lua/autorun/?.lua", "addons/*/lua/autorun/*", "lua/autorun", "lua/autoru*", "", "lua/weapons/*", "paths" };
	bool superset = true, skippedRejected = true;
	for (const char* p : patterns)
	{
		std::vector<int> visit = Walk(t, p);
		for (int i = 0; i < static_cast<int>(t.entries.size()); ++i)
		{
			const bool visited = std::find(visit.begin(), visit.end(), i) != visit.end();
			const bool match = GameMatch(p, t.entries[i].c_str());
			if (match && !visited)
				superset = false;
			if (!visited && match)
				skippedRejected = false;
		}
	}
	Check("every match is visited", superset);
	Check("every skipped entry is rejected by the matcher", skippedRejected);

	// Random paths and patterns: the same two properties.
	std::mt19937 rng(7);
	const char alphabet[] = "abAB/._*?";
	CNetworkStringTable r;
	for (int i = 0; i < 400; ++i)
	{
		std::string s;
		const int len = 1 + static_cast<int>(rng() % 12);
		for (int k = 0; k < len; ++k)
			s.push_back("abAB/._"[rng() % 7]);
		r.entries.push_back(s);
	}
	bool randomOk = true;
	for (int n = 0; n < 300 && randomOk; ++n)
	{
		std::string p;
		const int len = static_cast<int>(rng() % 8);
		for (int k = 0; k < len; ++k)
			p.push_back(alphabet[rng() % 9]);
		std::vector<int> visit = Walk(r, p.c_str());
		if (!std::is_sorted(visit.begin(), visit.end()))
			randomOk = false;
		for (int i = 0; i < static_cast<int>(r.entries.size()); ++i)
		{
			const bool visited = std::find(visit.begin(), visit.end(), i) != visit.end();
			if (GameMatch(p.c_str(), r.entries[i].c_str()) && !visited)
				randomOk = false;
		}
	}
	Check("random: every match visited, in order", randomOk);

	// The table grows (entries arrive while joining): seen at once.
	t.entries.push_back("lua/autorun/z.lua");
	w = Walk(t, "lua/autorun/*");
	Check("growth: the new entry is visited", !w.empty() && w.back() == 12);
	// A new join with a table at the same address and the same count: indexed again.
	t.entries[1] = "lua/other/b.lua";
	lua_find::NewJoin();
	w = Walk(t, "lua/autorun/*");
	Check("new join: indexed again", std::find(w.begin(), w.end(), 1) == w.end());

	Check("null table left to the game", Next(nullptr, "x", -1) == kLeaveToGame);
	Check("null pattern left to the game", Next(&t, nullptr, -1) == kLeaveToGame);
	CSomethingElse other;
	other.entries = { "lua/autorun/a.lua" };
	ResetForTest();
	SetEngineRangeForTest(base, size);
	Check("another class left to the game", Next(&other, "lua/*", -1) == kLeaveToGame);
	CNetworkStringTable empty;
	Check("an empty table left to the game", Next(&empty, "lua/*", -1) == kLeaveToGame);
	Check("status line", lua_find::Describe().find("table walks") != std::string::npos);
}

// The .text section of a DLL file, as raw bytes, and its RVA.
static bool FileText(const std::vector<uint8_t>& file, size_t& off, size_t& size, uint32_t& rva)
{
	if (file.size() < 0x200)
		return false;
	auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
	auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(file.data() + dos->e_lfanew);
	const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
	{
		if (std::memcmp(sec->Name, ".text", 6) == 0)
		{
			off = sec->PointerToRawData;
			size = sec->SizeOfRawData;
			rva = sec->VirtualAddress;
			return off + size <= file.size();
		}
	}
	return false;
}

static void Real(const char* gmod, const char* stubOut)
{
#ifdef _WIN64
	const std::string dll = std::string(gmod) + "\\bin\\win64\\client.dll";
#else
	const std::string dll = std::string(gmod) + "\\bin\\client.dll";
#endif
	std::ifstream f(dll, std::ios::binary);
	if (!f)
	{
		std::printf("skip: %s not found\n", dll.c_str());
		return;
	}
	std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	size_t off = 0, size = 0;
	uint32_t rva = 0;
	Check("client.dll .text", FileText(file, off, size, rva));
	const uint8_t* text = file.data() + off;
	const uintptr_t va = rva;
	size_t step = 0;
	std::string why;
	const long long start = lua_find::detail::FindSites(text, size, step, why);
	if (start < 0)
		std::printf("  why: %s\n", why.c_str());
	Check("the loop found once, and it agrees", start >= 0);
	if (start < 0)
		return;
	std::printf("  start at .text+%llX (RVA %llX), step at RVA %llX\n", start, start + va, static_cast<unsigned long long>(step + va));
	const uintptr_t imageBase = 0x10000000;
	const std::string stubs = lua_find::detail::BuildStubsForTest(text, size, imageBase + va, static_cast<size_t>(start), step,
		0x7FF000000000ull & static_cast<uintptr_t>(-1), 0x7FF000100000ull & static_cast<uintptr_t>(-1));
	Check("stubs built", !stubs.empty());
	if (stubOut && !stubs.empty())
	{
		std::ofstream o(stubOut, std::ios::binary);
		o.write(stubs.data(), static_cast<std::streamsize>(stubs.size()));
		std::printf("  stubs: %zu bytes -> %s\n", stubs.size(), stubOut);
	}
	// A near miss is refused: one byte changed in the start, then in the step.
	std::vector<uint8_t> copy(text, text + size);
	copy[static_cast<size_t>(start) + 1] ^= 0xFF;
	size_t unused = 0;
	Check("a changed byte in the start is refused", lua_find::detail::FindSites(copy.data(), copy.size(), unused, why) < 0);
	copy[static_cast<size_t>(start) + 1] ^= 0xFF;
	copy[step + 9] ^= 0xFF;
	Check("a changed byte in the step is refused", lua_find::detail::FindSites(copy.data(), copy.size(), unused, why) < 0);
}

int main(int argc, char** argv)
{
	Logic();
	if (argc > 1)
		Real(argv[1], argc > 2 ? argv[2] : nullptr);
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
