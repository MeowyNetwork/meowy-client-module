// The fast client Lua lookup, no game running:
//   lua_lookup_smoke [GarrysMod folder] [file to write the generated stub to]
// 1. Resolve() against a fake string table gives exactly what the game's
//    linear strcmp scan gives, and refuses anything it can't vouch for.
// 2. The patch site is found once in the installed client.dll of this
//    build's bitness, and a stub can be built for it (written out for the
//    offline disassembly check).
// Exit code 0 when every check passes.

#include "../src/lua_lookup.h"
#include "../src/pe_image.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
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
// 0..12), and the same class name as the engine's, so its RTTI matches.
class CNetworkStringTable
{
public:
	std::vector<std::string> entries;
	bool caseInsensitive = true;   // like a CUtlDict keyed without case
	bool brokenIndex = false;      // FindStringIndex lies about one entry
	int numStringsCalls = 0;

	virtual ~CNetworkStringTable() {}
	virtual const char* GetTableName() const { return "client_lua_files"; }
	virtual int GetTableId() const { return 7; }
	virtual int GetNumStrings() const { const_cast<CNetworkStringTable*>(this)->numStringsCalls++; return static_cast<int>(entries.size()); }
	virtual int GetMaxStrings() const { return 65536; }
	virtual int GetEntryBits() const { return 16; }
	virtual void SetTick(int) {}
	virtual bool ChangedSinceTick(int) const { return false; }
	virtual int AddString(bool, const char* value, int, const void*) { entries.push_back(value); return static_cast<int>(entries.size()) - 1; }
	virtual const char* GetString(int i) { return i >= 0 && i < static_cast<int>(entries.size()) ? entries[i].c_str() : nullptr; }
	virtual void SetStringUserData(int, int, const void*) {}
	virtual const void* GetStringUserData(int, int*) { return nullptr; }
	virtual int FindStringIndex(const char* s)
	{
		std::string key = Key(s);
		for (int i = 0; i < static_cast<int>(entries.size()); ++i)
		{
			if (Key(entries[i].c_str()) == key)
				return brokenIndex && i == 3 ? 2 : i;
		}
		return 65535;   // INVALID_STRING_INDEX
	}

private:
	std::string Key(const char* s) const
	{
		std::string k = s;
		if (caseInsensitive)
			for (char& c : k)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return k;
	}
};

// Same layout, another class: must be left to the game.
class CSomethingElse : public CNetworkStringTable
{
public:
	const char* GetTableName() const override { return "other"; }
};

// What the game's scan returns: the first entry equal byte for byte, or -1.
static int Scan(CNetworkStringTable& t, const char* name)
{
	for (int i = 0; i < static_cast<int>(t.entries.size()); ++i)
		if (std::strcmp(t.entries[i].c_str(), name) == 0)
			return i;
	return -1;
}

static int Index(int64_t r) { return static_cast<int32_t>(static_cast<uint32_t>(r)); }
static int Count(int64_t r) { return static_cast<int32_t>(static_cast<uint64_t>(r) >> 32); }

static void Logic()
{
	using namespace lua_lookup::detail;
	uintptr_t base = 0;
	size_t size = 0;
	pe_image::ImageRange(GetModuleHandleW(nullptr), base, size);
	SetEngineRangeForTest(base, size);
	ResetForTest();

	CNetworkStringTable t;
	t.entries = { "paths", "lua/includes/init.lua", "lua/autorun/a.lua", "gamemodes/darkrp/gamemode/cl_init.lua",
		"addons/x/lua/autorun/b.lua", "lua/weapons/m9k_ak47.lua", "lua/autorun/Mixed_Case.lua" };

	int64_t r = Resolve(&t, "lua/autorun/a.lua");
	Check("found: index", Index(r) == 2);
	Check("found: count", Count(r) == 7);
	Check("miss", Index(Resolve(&t, "lua/autorun/nope.lua")) == -1);
	Check("miss keeps the count", Count(Resolve(&t, "lua/autorun/nope.lua")) == 7);
	Check("case variant is a miss, like strcmp", Index(Resolve(&t, "LUA/AUTORUN/A.LUA")) == -1);
	Check("exact mixed case found", Index(Resolve(&t, "lua/autorun/Mixed_Case.lua")) == 6);
	Check("lower-cased mixed case is a miss", Index(Resolve(&t, "lua/autorun/mixed_case.lua")) == -1);
	Check("empty name is a miss", Index(Resolve(&t, "")) == -1);
	Check("null name left to the game", Resolve(&t, nullptr) == kLeaveToGame);
	Check("null table left to the game", Resolve(nullptr, "x") == kLeaveToGame);

	// Every entry, a case variant of each, and misses: the same answer as the scan.
	std::vector<std::string> names;
	for (const std::string& e : t.entries)
	{
		names.push_back(e);
		std::string up = e;
		for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		names.push_back(up);
		names.push_back(e + "x");
		names.push_back(e.substr(0, e.size() / 2));
	}
	bool same = true;
	for (const std::string& n : names)
		same = same && Index(Resolve(&t, n.c_str())) == Scan(t, n.c_str());
	Check("same answers as the scan", same);

	// A case-sensitive dictionary gives the same answers too.
	t.caseInsensitive = false;
	same = true;
	for (const std::string& n : names)
		same = same && Index(Resolve(&t, n.c_str())) == Scan(t, n.c_str());
	Check("same answers with a case-sensitive index", same);
	t.caseInsensitive = true;

	// Entries added later are checked and found.
	t.entries.push_back("lua/autorun/late.lua");
	Check("entry added later found", Index(Resolve(&t, "lua/autorun/late.lua")) == 7 && Count(Resolve(&t, "x")) == 8);

	// A table that shrinks (another server's table at the same address) is checked again.
	t.entries.resize(3);
	Check("shrunk table re-checked", Index(Resolve(&t, "lua/autorun/a.lua")) == 2 && Count(Resolve(&t, "x")) == 3);

	// Empty table: the game's own path.
	CNetworkStringTable empty;
	Check("empty table left to the game", Resolve(&empty, "x") == kLeaveToGame);

	// Another class with the same layout: left to the game.
	CSomethingElse other;
	other.entries = t.entries;
	Check("other class left to the game", Resolve(&other, "lua/autorun/a.lua") == kLeaveToGame);

	// Outside the engine's image: left to the game.
	SetEngineRangeForTest(1, 1);
	ResetForTest();
	Check("vtable outside the engine left to the game", Resolve(&t, "lua/autorun/a.lua") == kLeaveToGame);
	SetEngineRangeForTest(base, size);
	ResetForTest();

	// An index that misses an entry: refused for good.
	CNetworkStringTable bad;
	bad.entries = { "paths", "a.lua", "b.lua", "c.lua", "d.lua" };
	bad.brokenIndex = true;
	Check("broken index left to the game", Resolve(&bad, "a.lua") == kLeaveToGame);
	Check("and every table after it", Resolve(&t, "lua/autorun/a.lua") == kLeaveToGame);
	Check("refusal described", lua_lookup::Describe().find("left to the game:") != std::string::npos);
	ResetForTest();
	Check("reset", Index(Resolve(&t, "lua/autorun/a.lua")) == 2);
	Check("stats described", lua_lookup::Describe().find("1 found") != std::string::npos);
}

static std::vector<uint8_t> ReadFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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

static void Site(const std::string& gmod, const std::string& stubOut)
{
#ifdef _WIN64
	const std::string dll = gmod + "\\bin\\win64\\client.dll";
#else
	const std::string dll = gmod + "\\bin\\client.dll";
#endif
	std::vector<uint8_t> file = ReadFile(dll);
	if (file.empty())
	{
		std::printf("skip: %s not found\n", dll.c_str());
		return;
	}
	size_t off = 0, size = 0;
	uint32_t rva = 0;
	Check("client.dll has a .text section", FileText(file, off, size, rva));
	std::string why;
	const long long at = lua_lookup::detail::FindSite(file.data() + off, size, why);
	Check("patch site found once", at >= 0);
	if (at < 0)
	{
		std::printf("  %s\n", why.c_str());
		return;
	}
	std::printf("patch site: client.dll+0x%llX\n", static_cast<unsigned long long>(rva + at));

	// Built as if client.dll were at its preferred base and the stub right after it.
	const uintptr_t siteAt = static_cast<uintptr_t>(0x10000000) + rva + static_cast<uintptr_t>(at);
	const std::string stub = lua_lookup::detail::BuildStubForTest(file.data() + off + at, siteAt, 0x20000000, 0x30000000);
	Check("stub built", !stub.empty());
	if (!stubOut.empty() && !stub.empty())
	{
		std::ofstream(stubOut, std::ios::binary).write(stub.data(), static_cast<std::streamsize>(stub.size()));
		std::printf("stub: %zu bytes -> %s (site at 0x%llX, stub at 0x20000000, Resolve at 0x30000000)\n",
			stub.size(), stubOut.c_str(), static_cast<unsigned long long>(siteAt));
	}

	// The pattern rejects a near miss: one byte changed in the scan.
	std::vector<uint8_t> copy(file.begin() + static_cast<std::ptrdiff_t>(off), file.begin() + static_cast<std::ptrdiff_t>(off + size));
	copy[static_cast<size_t>(at) + 10] ^= 0xFF;
	Check("a changed byte is refused", lua_lookup::detail::FindSite(copy.data(), copy.size(), why) < 0);
}

int main(int argc, char** argv)
{
	Logic();
	const std::string gmod = argc > 1 ? argv[1] : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\GarrysMod";
	Site(gmod, argc > 2 ? argv[2] : "");
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
