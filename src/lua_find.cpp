#include "lua_find.h"

#include "pe_image.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	using lua_find::detail::kLeaveToGame;

	// --- The loop in client.dll (file.Find on the "LUA" path, GMod 2026.11.04 builds)
	//
	// For one search path's wildcard pattern (an MSVC std::string in the frame)
	// the game walks the client_lua_files table:
	//
	//   x86-64                                 x86
	//   start:                                 start:
	//   mov  r14,[rbp+h]     ; the data pack    mov  ecx,[ebx+4]     ; the table
	//   mov  ebx,r15d        ; i = 0            xor  esi,esi         ; i = 0
	//   mov  [rsp+c],ebx                        mov  [ebp+c],esi
	//   mov  rcx,[r14+8]     ; the table        mov  eax,[ecx]
	//   mov  rax,[rcx]                          call [eax+0Ch]       ; GetNumStrings
	//   call [rax+18h]       ; GetNumStrings    test eax,eax
	//   test eax,eax                            jle  exit2
	//   jle  exit2                              nop
	//   nop                                   top:
	// top:                                      ... GetString(i), copy into a std::string,
	//   ... GetString(i), copy into a           ... wildcard test(pattern, copy) ...
	//   ... std::string, wildcard test ...    step:
	// step:                                     mov  eax,[ebp+h]; mov esi,[ebp+c]; inc esi
	//   mov  rax,[rbp+h]; inc dword [rsp+c]     mov  [ebp+c],esi ... call [eax+0Ch]
	//   ... call [rax+18h]                      mov  ebx,[ebp+h]; cmp esi,eax
	//   mov  ebx,[rsp+c]; mov r14,[rbp+h]       jl   top
	//   cmp  ebx,eax; jl top                  exit1: mov edi,7FFFFFFFh
	// exit1: mov r12,7FFF...h                 exit2: ...
	// exit2: ...
	//
	// Both the start and the step jump to a stub that asks Next() for the next
	// entry that can pass the wildcard test, then goes to "top" with i set as
	// the loop would have it, or to "exit1" with i = the count, or runs the
	// replaced instructions (the game's own loop).
	constexpr int WILD = -1;
#if defined(_WIN64)
	const int kStart[] = {
		0x4C, 0x8B, 0x75, WILD,                  // mov  r14,[rbp+h]
		0x41, 0x8B, 0xDF,                        // mov  ebx,r15d
		0x89, 0x5C, 0x24, WILD,                  // mov  [rsp+c],ebx
		0x49, 0x8B, 0x4E, 0x08,                  // mov  rcx,[r14+8]
		0x48, 0x8B, 0x01,                        // mov  rax,[rcx]
		0xFF, 0x50, 0x18,                        // call [rax+18h]
		0x85, 0xC0,                              // test eax,eax
		0x0F, 0x8E, WILD, WILD, WILD, WILD,      // jle  exit2
		0x66, 0x90,                              // nop
		0x49, 0x8B, 0x4E, 0x08,                  // top: mov rcx,[r14+8]
		0x8B, 0xD3,                              // mov  edx,ebx
		0x48, 0x8B, 0x01,                        // mov  rax,[rcx]
		0xFF, 0x50, 0x48,                        // call [rax+48h]   GetString(i)
	};
	constexpr size_t kStartHolderAt = 3, kStartCounterAt = 10, kStartJleAt = 23, kStartKept = 23, kTop = 31;
	const int kStep[] = {
		0x48, 0x8B, 0x45, WILD,                  // mov  rax,[rbp+h]
		0xFF, 0x44, 0x24, WILD,                  // inc  dword [rsp+c]
		0x48, 0x8B, 0x48, 0x08,                  // mov  rcx,[rax+8]
		0x48, 0x8B, 0x01,                        // mov  rax,[rcx]
		0xFF, 0x50, 0x18,                        // call [rax+18h]
		0x8B, 0x5C, 0x24, WILD,                  // mov  ebx,[rsp+c]
		0x4C, 0x8B, 0x75, WILD,                  // mov  r14,[rbp+h]
		0x3B, 0xD8,                              // cmp  ebx,eax
		0x0F, 0x8C, WILD, WILD, WILD, WILD,      // jl   top
	};
	constexpr size_t kStepHolderAt = 3, kStepCounterAt = 7, kStepCounter2At = 21, kStepHolder2At = 25, kStepJlAt = 28, kStepKept = 28;
	const uint8_t kExit1[] = { 0x49, 0xBC };    // exit1: mov r12,imm64 (10 bytes), then exit2
	constexpr size_t kExit1Len = 10;
	// The wildcard test in the loop: lea rdx,[rbp+entry]; lea rcx,[rbp+pattern]; call wrapper; test al,al
	const int kTest[] = { 0x48, 0x8D, 0x55, WILD, 0x48, 0x8D, 0x4D, WILD, 0xE8, WILD, WILD, WILD, WILD, 0x84, 0xC0 };
	constexpr size_t kTestPatternAt = 7, kTestCallAt = 8;
	// The wrapper: both std::strings to char pointers, then the matcher.
	const uint8_t kWrapper[] = {
		0x48, 0x83, 0x7A, 0x18, 0x10, 0x72, 0x03, 0x48, 0x8B, 0x12,
		0x48, 0x83, 0x79, 0x18, 0x10, 0x72, 0x03, 0x48, 0x8B, 0x09, 0xE9,
	};
	// The matcher (pattern in rcx, string in rdx): until the first '*', each
	// pattern byte must equal the string's ('?' takes any); a mismatch returns 0.
	const int kMatcher[] = {
		0x33, 0xC0, 0x44, 0x8B, 0xD0,            // xor eax,eax; mov r10d,eax
		0x44, 0x0F, 0xB6, 0x0A,                  // movzx r9d,byte [rdx]
		0x45, 0x84, 0xC9, 0x74, WILD,            // test r9b,r9b; je end
		0x44, 0x0F, 0xB6, 0x01,                  // movzx r8d,byte [rcx]
		0x41, 0x80, 0xF8, 0x2A, 0x74, WILD,      // cmp r8b,'*'; je star
		0x45, 0x3A, 0xC1, 0x74, WILD,            // cmp r8b,r9b; je next
		0x41, 0x80, 0xF8, 0x3F, 0x75, WILD,      // cmp r8b,'?'; jne fail (eax = 0)
		0x48, 0xFF, 0xC1, 0x48, 0xFF, 0xC2,      // inc rcx; inc rdx
	};
	constexpr size_t kCap = 0x18;               // std::string capacity offset
#elif defined(_WIN32)
	const int kStart[] = {
		0x8B, 0x4B, 0x04,                        // mov  ecx,[ebx+4]
		0x33, 0xF6,                              // xor  esi,esi
		0x89, 0x75, WILD,                        // mov  [ebp+c],esi
		0x8B, 0x01,                              // mov  eax,[ecx]
		0xFF, 0x50, 0x0C,                        // call [eax+0Ch]
		0x85, 0xC0,                              // test eax,eax
		0x0F, 0x8E, WILD, WILD, WILD, WILD,      // jle  exit2
		0x66, 0x90,                              // nop
		0x8B, 0x4B, 0x04,                        // top: mov ecx,[ebx+4]
		0x56,                                    // push esi
		0x8B, 0x01,                              // mov  eax,[ecx]
		0xFF, 0x50, 0x24,                        // call [eax+24h]   GetString(i)
	};
	constexpr size_t kStartCounterAt = 7, kStartJleAt = 15, kStartKept = 15, kTop = 23;
	const int kStep[] = {
		0x8B, 0x45, WILD,                        // mov  eax,[ebp+h]
		0x8B, 0x75, WILD,                        // mov  esi,[ebp+c]
		0x46,                                    // inc  esi
		0x89, 0x75, WILD,                        // mov  [ebp+c],esi
		0x8B, 0x48, 0x04,                        // mov  ecx,[eax+4]
		0x8B, 0x01,                              // mov  eax,[ecx]
		0xFF, 0x50, 0x0C,                        // call [eax+0Ch]
		0x8B, 0x5D, WILD,                        // mov  ebx,[ebp+h]
		0x3B, 0xF0,                              // cmp  esi,eax
		0x0F, 0x8C, WILD, WILD, WILD, WILD,      // jl   top
	};
	constexpr size_t kStepHolderAt = 2, kStepCounterAt = 5, kStepCounter2At = 9, kStepHolder2At = 20, kStepJlAt = 23, kStepKept = 23;
	const uint8_t kExit1[] = { 0xBF };          // exit1: mov edi,imm32 (5 bytes), then exit2
	constexpr size_t kExit1Len = 5;
	// lea eax,[ebp+entry]; push eax; lea eax,[ebp+pattern]; push eax; call wrapper; add esp,8; test al,al
	const int kTest[] = { 0x8D, 0x45, WILD, 0x50, 0x8D, 0x85, WILD, WILD, WILD, WILD, 0x50,
		0xE8, WILD, WILD, WILD, WILD, 0x83, 0xC4, 0x08, 0x84, 0xC0 };
	constexpr size_t kTestPatternAt = 6, kTestCallAt = 11;
	const uint8_t kWrapper[] = {
		0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x83, 0x78, 0x14, 0x10, 0x72, 0x02, 0x8B, 0x00,
		0x8B, 0x4D, 0x08, 0x83, 0x79, 0x14, 0x10, 0x72, 0x02, 0x8B, 0x09,
		0x89, 0x45, 0x0C, 0x89, 0x4D, 0x08, 0x5D, 0xE9,
	};
	// The matcher (pattern first, string second): the same rules as on x86-64.
	const int kMatcher[] = {
		0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x8B, 0x45, 0x08,   // ecx = string, eax = pattern
		0x53, 0x56, 0x8A, 0x19, 0x33, 0xF6, 0x57, 0x33, 0xFF,
		0x84, 0xDB, 0x74, WILD,                  // test bl,bl; je end
		0x8A, 0x10,                              // mov dl,[eax]
		0x80, 0xFA, 0x2A, 0x74, WILD,            // cmp dl,'*'; je star
		0x3A, 0xD3, 0x74, WILD,                  // cmp dl,bl; je next
		0x80, 0xFA, 0x3F, 0x75, WILD,            // cmp dl,'?'; jne fail (returns 0)
	};
	constexpr size_t kCap = 0x14;
#endif

#ifdef _WIN32
	constexpr size_t kStartLen = sizeof(kStart) / sizeof(kStart[0]);
	constexpr size_t kStepLen = sizeof(kStep) / sizeof(kStep[0]);
	constexpr size_t kTestLen = sizeof(kTest) / sizeof(kTest[0]);
	constexpr size_t kMatcherLen = sizeof(kMatcher) / sizeof(kMatcher[0]);
	// Bytes replaced: the start through "nop" (up to top), and the whole step.
	constexpr size_t kStartReplaced = kTop;
	constexpr size_t kStepReplaced = kStepLen;

	int32_t Rel32(const uint8_t* p)
	{
		int32_t v;
		std::memcpy(&v, p, sizeof(v));
		return v;
	}

	template <size_t N>
	bool Matches(const uint8_t* p, const int (&pattern)[N])
	{
		for (size_t i = 0; i < N; ++i)
			if (pattern[i] != WILD && p[i] != static_cast<uint8_t>(pattern[i]))
				return false;
		return true;
	}

	// The two sites and what the stubs need from them.
	struct Sites
	{
		size_t start = 0, step = 0;      // offsets in the code section
		size_t top = 0, exit1 = 0, exit2 = 0;
		int8_t counter = 0;              // [rsp+c] / [ebp+c]
		int8_t holder = 0;               // [rbp+h] / [ebp+h]
		int32_t pattern = 0;             // [rbp+p] / [ebp+p]: the pattern std::string
	};

	bool Decode(const uint8_t* text, size_t size, size_t start, size_t step, Sites& s, std::string& why)
	{
		const uint8_t* a = text + start;
		const uint8_t* b = text + step;
		s.start = start;
		s.step = step;
		s.top = start + kTop;
		s.exit1 = step + kStepLen;
		s.exit2 = s.exit1 + kExit1Len;
		s.counter = static_cast<int8_t>(b[kStepCounterAt]);
		s.holder = static_cast<int8_t>(b[kStepHolderAt]);
		const size_t jle = start + kStartJleAt + 6 + Rel32(a + kStartJleAt + 2);
		const size_t jl = step + kStepJlAt + 6 + Rel32(b + kStepJlAt + 2);
#if defined(_WIN64)
		if (static_cast<int8_t>(a[kStartHolderAt]) != s.holder)
		{
			why = "the loop's start and step keep the data pack in different places";
			return false;
		}
#endif
		if (static_cast<int8_t>(a[kStartCounterAt]) != s.counter || static_cast<int8_t>(b[kStepCounter2At]) != s.counter ||
			static_cast<int8_t>(b[kStepHolder2At]) != s.holder)
		{
			why = "the loop's counter isn't kept in one place";
			return false;
		}
		if (jl != s.top || jle != s.exit2 || s.exit2 >= size ||
			std::memcmp(text + s.exit1, kExit1, sizeof(kExit1)) != 0)
		{
			why = "the loop's start, step and exits don't agree";
			return false;
		}
		// The wildcard test: the first one after "top".
		size_t test = 0;
		for (size_t i = s.top; i + kTestLen <= s.step; ++i)
		{
			if (Matches(text + i, kTest))
			{
				test = i;
				break;
			}
		}
		if (!test)
		{
			why = "the loop's wildcard test wasn't found";
			return false;
		}
#if defined(_WIN64)
		s.pattern = static_cast<int8_t>(text[test + kTestPatternAt]);
#else
		s.pattern = Rel32(text + test + kTestPatternAt);
#endif
		const long long wrapper = static_cast<long long>(test + kTestCallAt) + 5 + Rel32(text + test + kTestCallAt + 1);
		if (wrapper < 0 || static_cast<size_t>(wrapper) + sizeof(kWrapper) + 4 > size ||
			std::memcmp(text + wrapper, kWrapper, sizeof(kWrapper)) != 0)
		{
			why = "the loop's wildcard test isn't the expected wrapper";
			return false;
		}
		const size_t jmpAt = static_cast<size_t>(wrapper) + sizeof(kWrapper) - 1;   // the wrapper's final jmp
		const long long matcher = static_cast<long long>(jmpAt) + 5 + Rel32(text + jmpAt + 1);
		if (matcher < 0 || static_cast<size_t>(matcher) + kMatcherLen > size)
		{
			why = "the wildcard matcher is outside the code section";
			return false;
		}
		for (size_t i = 0; i < kMatcherLen; ++i)
		{
			if (kMatcher[i] != WILD && text[matcher + i] != static_cast<uint8_t>(kMatcher[i]))
			{
				why = "the wildcard matcher isn't the expected one";
				return false;
			}
		}
		return true;
	}

	// --- The table: public Source SDK INetworkStringTable slots 3 and 9 ---------
#ifdef _WIN64
	using GetNumStringsFn = int (*)(void*);
	using GetStringFn = const char* (*)(void*, int);
#else
	using GetNumStringsFn = int(__thiscall*)(void*);
	using GetStringFn = const char*(__thiscall*)(void*, int);
#endif
	template <class Fn>
	Fn Slot(void* table, int slot) { return reinterpret_cast<Fn>((*reinterpret_cast<void***>(table))[slot]); }
	int GetNumStrings(void* t) { return Slot<GetNumStringsFn>(t, 3)(t); }
	const char* GetString(void* t, int i) { return Slot<GetStringFn>(t, 9)(t, i); }

	// --- State --------------------------------------------------------------------
	std::mutex g_mutex;
	uintptr_t g_engineBase = 0;
	size_t g_engineSize = 0;
	void* g_goodVtable = nullptr;

	struct Entry
	{
		std::string s;
		int index;
	};
	std::vector<Entry> g_sorted;         // the table's entries, by byte order
	std::vector<size_t> g_where;         // index -> its place in g_sorted
	// The table the copy was made from, as lua_lookup tells tables apart: its
	// address, its first entry's own pointer (a new table at an old one's
	// address has its own strings) and how many entries the copy has.
	void* g_indexTable = nullptr;
	const char* g_indexFirst = nullptr;
	int g_indexCount = -1;
	bool g_dirty = true;                 // a join or changelevel started: check every entry again
	std::string g_candFor;               // the pattern the candidates are for
	std::vector<int> g_cand;             // its candidates, by index
	bool g_candValid = false;

	std::atomic<long long> g_loops{ 0 }, g_skipped{ 0 }, g_toGame{ 0 };
	// Entries read to check or (re)build the copy, and what checking every
	// entry at the start of every search (as it used to) would have read:
	// loops x count. The difference is the work saved.
	std::atomic<long long> g_checked{ 0 }, g_perSearch{ 0 };

	uint8_t* g_startSite = nullptr;      // patched, or null
	uint8_t* g_stepSite = nullptr;
	uint8_t g_startOriginal[kStartReplaced];
	uint8_t g_stepOriginal[kStepReplaced];
	uint8_t* g_stub = nullptr;

	bool InEngine(const void* p, size_t len)
	{
		auto a = reinterpret_cast<uintptr_t>(p);
		return g_engineSize && a >= g_engineBase && a + len <= g_engineBase + g_engineSize && a + len >= a;
	}

	// The table's class, from MSVC RTTI (as lua_lookup checks it).
	bool IsStringTable(void* table)
	{
		void** vtable = *reinterpret_cast<void***>(table);
		if (vtable == g_goodVtable)
			return true;
		if (!InEngine(vtable - 1, sizeof(void*)))
			return false;
		auto* col = static_cast<const uint32_t*>(vtable[-1]);
		if (!InEngine(col, 6 * sizeof(uint32_t)))
			return false;
#ifdef _WIN64
		if (col[0] != 1)
			return false;
		const uintptr_t image = reinterpret_cast<uintptr_t>(col) - col[5];
		auto* name = reinterpret_cast<const char*>(image + col[3] + 16);
#else
		if (col[0] != 0)
			return false;
		auto* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(col[3]) + 8);
#endif
		static const char kName[] = ".?AVCNetworkStringTable@@";
		if (!InEngine(name, sizeof(kName)) || std::memcmp(name, kName, sizeof(kName)) != 0)
			return false;
		g_goodVtable = vtable;
		return true;
	}

	// Every entry still what the sorted copy has at its index.
	bool SameEntries(void* table, int count)
	{
		if (g_where.size() != static_cast<size_t>(count) || g_sorted.size() != static_cast<size_t>(count))
			return false;
		g_checked += count;
		for (int i = 0; i < count; ++i)
		{
			const char* s = GetString(table, i);
			if (!s || std::strcmp(s, g_sorted[g_where[i]].s.c_str()) != 0)
				return false;
		}
		return true;
	}

	bool EntryBefore(const Entry& x, const Entry& y)
	{
		const int c = std::strcmp(x.s.c_str(), y.s.c_str());
		return c != 0 ? c < 0 : x.index < y.index;
	}

	void Where(int count)
	{
		g_where.assign(static_cast<size_t>(count), 0);
		for (size_t p = 0; p < g_sorted.size(); ++p)
			g_where[static_cast<size_t>(g_sorted[p].index)] = p;
	}

	void Adopt(void* table, const char* first, int count)
	{
		g_indexTable = table;
		g_indexFirst = first;
		g_indexCount = count;
		g_dirty = false;
		g_candValid = false;
	}

	// The whole sorted copy, made again.
	bool Rebuild(void* table, int count, const char* first)
	{
		g_dirty = true;
		g_sorted.clear();
		g_where.clear();
		g_sorted.reserve(static_cast<size_t>(count));
		g_checked += count;
		for (int i = 0; i < count; ++i)
		{
			const char* s = GetString(table, i);
			if (!s)
				return false;
			g_sorted.push_back({ s, i });
		}
		std::sort(g_sorted.begin(), g_sorted.end(), EntryBefore);
		Where(count);
		Adopt(table, first, count);
		return true;
	}

	// The same table with entries added since (they arrive while joining):
	// only the new ones are read, sorted and merged into the copy.
	bool Grow(void* table, int count, const char* first)
	{
		const int from = g_indexCount;
		g_dirty = true;   // until the copy is whole again
		std::vector<Entry> added;
		added.reserve(static_cast<size_t>(count - from));
		g_checked += count - from;
		for (int i = from; i < count; ++i)
		{
			const char* s = GetString(table, i);
			if (!s)
				return false;
			added.push_back({ s, i });
		}
		std::sort(added.begin(), added.end(), EntryBefore);
		const size_t mid = g_sorted.size();
		g_sorted.insert(g_sorted.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
		std::inplace_merge(g_sorted.begin(), g_sorted.begin() + static_cast<std::ptrdiff_t>(mid), g_sorted.end(), EntryBefore);
		Where(count);
		Adopt(table, first, count);
		return true;
	}

	// The sorted copy for this table. Network string tables only ever grow
	// within a connection and their names never change, so a table is known
	// by its address, its first entry's pointer and its count, the way
	// lua_lookup knows it: the same table costs nothing, and entries added
	// since are merged in. Every entry is compared with the copy again only
	// when that identity changes or a join started (NewJoin, at every loading
	// start, changelevel included: a map change makes a new table, which can
	// land at the old one's address with the same count but other files).
	// The copy is kept when every entry still matches, else made again.
	bool Indexed(void* table, int count)
	{
		const char* first = GetString(table, 0);
		if (!first)
		{
			g_dirty = true;
			return false;
		}
		const bool same = table == g_indexTable && first == g_indexFirst;
		if (!g_dirty && same && count == g_indexCount)
			return true;
		if (!g_dirty && same && count > g_indexCount)
			return Grow(table, count, first);
		if (count == g_indexCount && SameEntries(table, count))
		{
			Adopt(table, first, count);
			return true;
		}
		return Rebuild(table, count, first);
	}

	// The entries starting with the pattern's literal part, by index.
	void Candidates(const char* pattern)
	{
		if (g_candValid && g_candFor == pattern)
			return;
		g_candFor = pattern;
		g_cand.clear();
		const std::string lit = lua_find::detail::LiteralPart(pattern);
		auto it = std::lower_bound(g_sorted.begin(), g_sorted.end(), lit, [](const Entry& e, const std::string& key) {
			return std::strcmp(e.s.c_str(), key.c_str()) < 0;
		});
		for (; it != g_sorted.end() && it->s.compare(0, lit.size(), lit) == 0; ++it)
			g_cand.push_back(it->index);
		std::sort(g_cand.begin(), g_cand.end());
		g_candValid = true;
	}

	// --- The stubs ------------------------------------------------------------------
	struct Code
	{
		std::vector<uint8_t> b;
		uintptr_t at = 0;

		void Put(std::initializer_list<uint8_t> bytes) { b.insert(b.end(), bytes); }
		void PutBytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
		template <class T> void Imm(T v)
		{
			uint8_t raw[sizeof(T)];
			std::memcpy(raw, &v, sizeof(T));
			b.insert(b.end(), raw, raw + sizeof(T));
		}
		size_t Fixup() { size_t p = b.size(); Imm<int32_t>(0); return p; }
		void Bind(size_t fixup) { int32_t rel = static_cast<int32_t>(b.size() - (fixup + 4)); std::memcpy(&b[fixup], &rel, 4); }
		void JmpTo(uintptr_t target)
		{
#ifdef _WIN64
			Put({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });   // jmp [rip+0]
			Imm<uint64_t>(target);
#else
			Put({ 0xE9 });
			Imm<int32_t>(static_cast<int32_t>(target - (at + b.size() + 4)));
#endif
		}
		void Align16() { while ((at + b.size()) % 16) Put({ 0xCC }); }
	};

#ifdef _WIN64
	// rdx = the pattern's characters: inline, or on the heap past the SSO capacity.
	void PatternToRdx(Code& c, int32_t p)
	{
		c.Put({ 0x48, 0x8D, 0x95 }); c.Imm<int32_t>(p);                     // lea  rdx,[rbp+p]
		c.Put({ 0x48, 0x83, 0xBD }); c.Imm<int32_t>(p + static_cast<int32_t>(kCap)); c.Put({ 0x10 }); // cmp qword [rbp+p+cap],10h
		c.Put({ 0x48, 0x0F, 0x43, 0x95 }); c.Imm<int32_t>(p);               // cmovae rdx,[rbp+p]
	}
	void CallNext(Code& c, uintptr_t nextAt)
	{
		c.Put({ 0x48, 0xB8 }); c.Imm<uint64_t>(nextAt);                     // mov  rax,Next
		c.Put({ 0xFF, 0xD0 });                                               // call rax
	}
#else
	void PatternToEax(Code& c, int32_t p)
	{
		c.Put({ 0x8D, 0x85 }); c.Imm<int32_t>(p);                            // lea  eax,[ebp+p]
		c.Put({ 0x83, 0xBD }); c.Imm<int32_t>(p + static_cast<int32_t>(kCap)); c.Put({ 0x10 }); // cmp dword [ebp+p+cap],10h
		c.Put({ 0x0F, 0x43, 0x85 }); c.Imm<int32_t>(p);                      // cmovae eax,[ebp+p]
	}
	void CallNext(Code& c, uintptr_t nextAt)
	{
		c.Put({ 0xB8 }); c.Imm<uint32_t>(static_cast<uint32_t>(nextAt));   // mov  eax,Next
		c.Put({ 0xFF, 0xD0 });                                               // call eax   (cdecl)
		c.Put({ 0x83, 0xC4, 0x0C });                                         // add  esp,12
	}
#endif

	// The stub the start jumps to.
	std::vector<uint8_t> BuildStart(const uint8_t* text, uintptr_t textAt, const Sites& s, uintptr_t stubAt, uintptr_t nextAt)
	{
		const uint8_t c8 = static_cast<uint8_t>(s.counter), h8 = static_cast<uint8_t>(s.holder);
		Code c;
		c.at = stubAt;
#ifdef _WIN64
		c.Put({ 0x4C, 0x8B, 0x75, h8 });                     // mov  r14,[rbp+h]      the data pack
		c.Put({ 0x49, 0x8B, 0x4E, 0x08 });                   // mov  rcx,[r14+8]      table
		PatternToRdx(c, s.pattern);                          //      rdx = pattern
		c.Put({ 0x41, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF });       // mov  r8d,-1           after
		CallNext(c, nextAt);
		c.Put({ 0x83, 0xF8, 0xFE });                         // cmp  eax,-2
		c.Put({ 0x0F, 0x84 }); size_t toGame = c.Fixup();    // je   game
		c.Put({ 0x83, 0xF8, 0xFF });                         // cmp  eax,-1
		c.Put({ 0x0F, 0x84 }); size_t toNone = c.Fixup();    // je   none
		c.Put({ 0x8B, 0xD8 });                               // mov  ebx,eax          i
		c.Put({ 0x89, 0x5C, 0x24, c8 });                     // mov  [rsp+c],ebx
		c.JmpTo(textAt + s.top);
		c.Bind(toNone);                                      // none: i = count, as the loop leaves it
		c.Put({ 0x48, 0xC1, 0xE8, 0x20 });                   // shr  rax,32
		c.Put({ 0x8B, 0xD8 });                               // mov  ebx,eax
		c.Put({ 0x89, 0x5C, 0x24, c8 });                     // mov  [rsp+c],ebx
		c.JmpTo(textAt + s.exit1);
		c.Bind(toGame);                                      // game: the replaced instructions
		c.PutBytes(text + s.start, kStartKept);              //      through "test eax,eax"
		c.Put({ 0x0F, 0x8E }); size_t toExit2 = c.Fixup();   // jle  exit2
		c.JmpTo(textAt + s.top);
		c.Bind(toExit2);
		c.JmpTo(textAt + s.exit2);
#else
		c.Put({ 0x8B, 0x4B, 0x04 });                         // mov  ecx,[ebx+4]      table
		PatternToEax(c, s.pattern);                          //      eax = pattern
		c.Put({ 0x6A, 0xFF });                               // push -1               after
		c.Put({ 0x50 });                                     // push eax
		c.Put({ 0x51 });                                     // push ecx
		CallNext(c, nextAt);
		c.Put({ 0x83, 0xF8, 0xFE });                         // cmp  eax,-2
		c.Put({ 0x0F, 0x84 }); size_t toGame = c.Fixup();    // je   game
		c.Put({ 0x83, 0xF8, 0xFF });                         // cmp  eax,-1
		c.Put({ 0x0F, 0x84 }); size_t toNone = c.Fixup();    // je   none
		c.Put({ 0x8B, 0xF0 });                               // mov  esi,eax          i
		c.Put({ 0x89, 0x75, c8 });                           // mov  [ebp+c],esi
		c.JmpTo(textAt + s.top);
		c.Bind(toNone);                                      // none: i = count
		c.Put({ 0x8B, 0xF2 });                               // mov  esi,edx
		c.Put({ 0x89, 0x75, c8 });                           // mov  [ebp+c],esi
		c.Put({ 0x8B, 0x5D, h8 });                           // mov  ebx,[ebp+h]      as the step leaves it
		c.JmpTo(textAt + s.exit1);
		c.Bind(toGame);
		c.PutBytes(text + s.start, kStartKept);              // the replaced instructions through "test eax,eax"
		c.Put({ 0x0F, 0x8E }); size_t toExit2 = c.Fixup();   // jle  exit2
		c.JmpTo(textAt + s.top);
		c.Bind(toExit2);
		c.JmpTo(textAt + s.exit2);
#endif
		return c.b;
	}

	// The stub the step jumps to.
	std::vector<uint8_t> BuildStep(const uint8_t* text, uintptr_t textAt, const Sites& s, uintptr_t stubAt, uintptr_t nextAt)
	{
		const uint8_t c8 = static_cast<uint8_t>(s.counter), h8 = static_cast<uint8_t>(s.holder);
		Code c;
		c.at = stubAt;
#ifdef _WIN64
		c.Put({ 0x4C, 0x8B, 0x75, h8 });                     // mov  r14,[rbp+h]
		c.Put({ 0x49, 0x8B, 0x4E, 0x08 });                   // mov  rcx,[r14+8]      table
		PatternToRdx(c, s.pattern);
		c.Put({ 0x44, 0x8B, 0x44, 0x24, c8 });               // mov  r8d,[rsp+c]      after = i
		CallNext(c, nextAt);
		c.Put({ 0x4C, 0x8B, 0x75, h8 });                     // mov  r14,[rbp+h]      as the step leaves it
		c.Put({ 0x83, 0xF8, 0xFE });                         // cmp  eax,-2
		c.Put({ 0x0F, 0x84 }); size_t toGame = c.Fixup();    // je   game
		c.Put({ 0x83, 0xF8, 0xFF });                         // cmp  eax,-1
		c.Put({ 0x0F, 0x84 }); size_t toNone = c.Fixup();    // je   none
		c.Put({ 0x8B, 0xD8 });                               // mov  ebx,eax
		c.Put({ 0x89, 0x5C, 0x24, c8 });                     // mov  [rsp+c],ebx
		c.JmpTo(textAt + s.top);
		c.Bind(toNone);
		c.Put({ 0x48, 0xC1, 0xE8, 0x20 });                   // shr  rax,32
		c.Put({ 0x8B, 0xD8 });                               // mov  ebx,eax          i = count
		c.Put({ 0x89, 0x5C, 0x24, c8 });                     // mov  [rsp+c],ebx
		c.JmpTo(textAt + s.exit1);
		c.Bind(toGame);
		c.PutBytes(text + s.step, kStepKept);                // the replaced instructions through "cmp ebx,eax"
		c.Put({ 0x0F, 0x8C }); size_t toTop = c.Fixup();     // jl   top
		c.JmpTo(textAt + s.exit1);
		c.Bind(toTop);
		c.JmpTo(textAt + s.top);
#else
		c.Put({ 0x8B, 0x5D, h8 });                           // mov  ebx,[ebp+h]
		c.Put({ 0x8B, 0x4B, 0x04 });                         // mov  ecx,[ebx+4]      table
		PatternToEax(c, s.pattern);
		c.Put({ 0xFF, 0x75, c8 });                           // push dword [ebp+c]    after = i
		c.Put({ 0x50 });                                     // push eax
		c.Put({ 0x51 });                                     // push ecx
		CallNext(c, nextAt);
		c.Put({ 0x8B, 0x5D, h8 });                           // mov  ebx,[ebp+h]      as the step leaves it
		c.Put({ 0x83, 0xF8, 0xFE });                         // cmp  eax,-2
		c.Put({ 0x0F, 0x84 }); size_t toGame = c.Fixup();    // je   game
		c.Put({ 0x83, 0xF8, 0xFF });                         // cmp  eax,-1
		c.Put({ 0x0F, 0x84 }); size_t toNone = c.Fixup();    // je   none
		c.Put({ 0x8B, 0xF0 });                               // mov  esi,eax
		c.Put({ 0x89, 0x75, c8 });                           // mov  [ebp+c],esi
		c.JmpTo(textAt + s.top);
		c.Bind(toNone);
		c.Put({ 0x8B, 0xF2 });                               // mov  esi,edx          i = count
		c.Put({ 0x89, 0x75, c8 });                           // mov  [ebp+c],esi
		c.JmpTo(textAt + s.exit1);
		c.Bind(toGame);
		c.PutBytes(text + s.step, kStepKept);                // the replaced instructions through "cmp esi,eax"
		c.Put({ 0x0F, 0x8C }); size_t toTop = c.Fixup();     // jl   top
		c.JmpTo(textAt + s.exit1);
		c.Bind(toTop);
		c.JmpTo(textAt + s.top);
#endif
		return c.b;
	}

	// Both stubs, the step's on a 16-byte boundary after the start's.
	std::vector<uint8_t> BuildStubs(const uint8_t* text, uintptr_t textAt, const Sites& s, uintptr_t stubAt,
		uintptr_t nextAt, size_t& stepStubOffset)
	{
		std::vector<uint8_t> out = BuildStart(text, textAt, s, stubAt, nextAt);
		while ((stubAt + out.size()) % 16)
			out.push_back(0xCC);
		stepStubOffset = out.size();
		const std::vector<uint8_t> step = BuildStep(text, textAt, s, stubAt + stepStubOffset, nextAt);
		out.insert(out.end(), step.begin(), step.end());
		return out;
	}

	// A jump from a site to a stub, padded with int3 to `len` bytes.
	std::vector<uint8_t> Jump(uintptr_t from, uintptr_t to, size_t len)
	{
		std::vector<uint8_t> j;
#ifdef _WIN64
		j = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };          // jmp [rip+0]
		uint8_t raw[8];
		const uint64_t target = to;
		std::memcpy(raw, &target, 8);
		j.insert(j.end(), raw, raw + 8);
		(void)from;
#else
		j = { 0xE9 };
		const int32_t rel = static_cast<int32_t>(to - (from + 5));
		uint8_t raw[4];
		std::memcpy(raw, &rel, 4);
		j.insert(j.end(), raw, raw + 4);
#endif
		while (j.size() < len)
			j.push_back(0xCC);
		return j;
	}

	bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t len)
	{
		DWORD old;
		if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old))
			return false;
		std::memcpy(at, bytes, len);
		DWORD unused;
		VirtualProtect(at, len, old, &unused);
		FlushInstructionCache(GetCurrentProcess(), at, len);
		return true;
	}
#endif
}

// Called by the stubs, from the middle of the game's loop.
#ifdef _WIN32
extern "C" int64_t __cdecl MeowyLuaFindNext(void* table, const char* pattern, int after) noexcept
{
	return lua_find::detail::Next(table, pattern, after);
}
#endif

namespace lua_find
{
	namespace detail
	{
		std::string LiteralPart(const char* pattern)
		{
			std::string out;
			if (!pattern)
				return out;
			for (const char* p = pattern; *p && *p != '*' && *p != '?'; ++p)
				out.push_back(*p);
			return out;
		}

		int64_t Next(void* table, const char* pattern, int after) noexcept
		{
#ifdef _WIN32
			if (!table || !pattern)
			{
				++g_toGame;
				return kLeaveToGame;
			}
			try
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (!IsStringTable(table))
				{
					++g_toGame;
					return kLeaveToGame;
				}
				const int count = GetNumStrings(table);
				if (count <= 0 || !Indexed(table, count))
				{
					++g_toGame;
					return kLeaveToGame;
				}
				Candidates(pattern);
				if (after < 0)
				{
					++g_loops;
					g_perSearch += count;
					g_skipped += count - static_cast<long long>(g_cand.size());
				}
				auto it = std::upper_bound(g_cand.begin(), g_cand.end(), after);
				const int index = it == g_cand.end() ? -1 : *it;
				return (static_cast<int64_t>(static_cast<uint32_t>(count)) << 32) | static_cast<uint32_t>(index);
			}
			catch (...)
			{
				// Out of memory for the copy: the game's own loop, this time,
				// and a whole new copy next time.
				g_dirty = true;
				g_sorted.clear();
				g_where.clear();
				g_indexCount = -1;
				++g_toGame;
				return kLeaveToGame;
			}
#else
			(void)table; (void)pattern; (void)after;
			return kLeaveToGame;
#endif
		}

		void SetEngineRangeForTest(uintptr_t base, size_t size)
		{
#ifdef _WIN32
			std::lock_guard<std::mutex> lock(g_mutex);
			g_engineBase = base;
			g_engineSize = size;
#else
			(void)base; (void)size;
#endif
		}

		long long CheckedForTest()
		{
#ifdef _WIN32
			return g_checked.load();
#else
			return 0;
#endif
		}

		void ResetForTest()
		{
#ifdef _WIN32
			std::lock_guard<std::mutex> lock(g_mutex);
			g_goodVtable = nullptr;
			g_sorted.clear();
			g_where.clear();
			g_indexTable = nullptr;
			g_indexCount = -1;
			g_indexFirst = nullptr;
			g_dirty = true;
			g_candValid = false;
			g_loops = g_skipped = g_toGame = 0;
			g_checked = g_perSearch = 0;
#endif
		}

		long long FindSites(const uint8_t* text, size_t size, size_t& step, std::string& why)
		{
#ifdef _WIN32
			long long start = -1, stepAt = -1;
			int starts = 0, steps = 0;
			for (size_t i = 0; i + kStartLen <= size; ++i)
			{
				if (Matches(text + i, kStart)) { start = static_cast<long long>(i); ++starts; }
				if (i + kStepLen <= size && Matches(text + i, kStep)) { stepAt = static_cast<long long>(i); ++steps; }
			}
			if (starts != 1 || steps != 1)
			{
				why = starts == 0 || steps == 0 ? "the file.Find loop wasn't found (different GMod build)"
				                                : "the file.Find loop was found more than once";
				return -1;
			}
			if (stepAt <= start || stepAt - start > 0x4000)
			{
				why = "the loop's step isn't after its start";
				return -1;
			}
			Sites s;
			if (!Decode(text, size, static_cast<size_t>(start), static_cast<size_t>(stepAt), s, why))
				return -1;
			step = static_cast<size_t>(stepAt);
			return start;
#else
			(void)text; (void)size; (void)step;
			why = "Windows only";
			return -1;
#endif
		}

		std::string BuildStubsForTest(const uint8_t* text, size_t size, uintptr_t textAt, size_t start, size_t step,
			uintptr_t stubAt, uintptr_t nextAt)
		{
#ifdef _WIN32
			Sites s;
			std::string why;
			if (!Decode(text, size, start, step, s, why))
				return {};
			size_t stepStub = 0;
			const std::vector<uint8_t> b = BuildStubs(text, textAt, s, stubAt, nextAt, stepStub);
			return std::string(b.begin(), b.end());
#else
			(void)text; (void)size; (void)textAt; (void)start; (void)step; (void)stubAt; (void)nextAt;
			return {};
#endif
		}
	}

	bool Install(std::string& why)
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_startSite)
			return true;

		HMODULE client = GetModuleHandleW(L"client.dll");
		HMODULE engine = GetModuleHandleW(L"engine.dll");
		uint8_t* text = nullptr;
		size_t size = 0;
		if (!client || !pe_image::TextSection(client, text, size))
		{
			why = "client.dll not found";
			return false;
		}
		if (!g_engineSize && (!engine || !pe_image::ImageRange(engine, g_engineBase, g_engineSize)))
		{
			why = "engine.dll not found";
			return false;
		}
		size_t step = 0;
		const long long start = detail::FindSites(text, size, step, why);
		if (start < 0)
			return false;
		Sites s;
		if (!Decode(text, size, static_cast<size_t>(start), step, s, why))
			return false;

		if (!g_stub)
		{
			g_stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
			if (!g_stub)
			{
				why = "couldn't allocate the stubs";
				return false;
			}
		}
		const uintptr_t textAt = reinterpret_cast<uintptr_t>(text);
		size_t stepStub = 0;
		const std::vector<uint8_t> stubs = BuildStubs(text, textAt, s, reinterpret_cast<uintptr_t>(g_stub),
			reinterpret_cast<uintptr_t>(&MeowyLuaFindNext), stepStub);
		DWORD old;
		if (stubs.size() > 4096 || !VirtualProtect(g_stub, 4096, PAGE_READWRITE, &old))
		{
			why = "couldn't write the stubs";
			return false;
		}
		std::memcpy(g_stub, stubs.data(), stubs.size());
		if (!VirtualProtect(g_stub, 4096, PAGE_EXECUTE_READ, &old))
		{
			why = "couldn't make the stubs executable";
			return false;
		}
		FlushInstructionCache(GetCurrentProcess(), g_stub, stubs.size());

		uint8_t* startSite = text + s.start;
		uint8_t* stepSite = text + s.step;
		const std::vector<uint8_t> jStart = Jump(reinterpret_cast<uintptr_t>(startSite), reinterpret_cast<uintptr_t>(g_stub), kStartReplaced);
		const std::vector<uint8_t> jStep = Jump(reinterpret_cast<uintptr_t>(stepSite), reinterpret_cast<uintptr_t>(g_stub) + stepStub, kStepReplaced);
		std::memcpy(g_startOriginal, startSite, kStartReplaced);
		std::memcpy(g_stepOriginal, stepSite, kStepReplaced);
		// The step first: until the start is patched, the game never reaches it
		// through a stub, and its own start works with either step.
		if (!WriteCode(stepSite, jStep.data(), kStepReplaced))
		{
			why = "couldn't write the patch";
			return false;
		}
		if (!WriteCode(startSite, jStart.data(), kStartReplaced))
		{
			WriteCode(stepSite, g_stepOriginal, kStepReplaced);
			why = "couldn't write the patch";
			return false;
		}
		g_startSite = startSite;
		g_stepSite = stepSite;
		g_dirty = true;
		return true;
#else
		why = "Windows only";
		return false;
#endif
	}

	bool Uninstall()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_startSite)
			return false;
		if (!WriteCode(g_startSite, g_startOriginal, kStartReplaced) || !WriteCode(g_stepSite, g_stepOriginal, kStepReplaced))
			return false;
		// The stubs stay allocated: tiny, and nothing can be running in them here
		// (only the main thread runs the loop, and it's here).
		g_startSite = g_stepSite = nullptr;
		return true;
#else
		return false;
#endif
	}

	void NewJoin()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		g_dirty = true;
		g_candValid = false;
#endif
	}

	bool Installed()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_startSite != nullptr;
#else
		return false;
#endif
	}

	std::string Describe()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		std::string out = g_startSite ? "on" : "off";
		out += ": " + std::to_string(g_loops.load()) + " table walks, " + std::to_string(g_skipped.load()) +
			" entries skipped, " + std::to_string(g_toGame.load()) + " left to the game's own loop, " +
			std::to_string(g_checked.load()) + " entries checked (every entry at every search: " +
			std::to_string(g_perSearch.load()) + ")";
		return out;
#else
		return "unsupported";
#endif
	}
}
