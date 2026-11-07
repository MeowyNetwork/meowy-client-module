#include "lua_lookup.h"

#include <atomic>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include "pe_image.h"
#endif

namespace
{
	using lua_lookup::detail::kLeaveToGame;

	// --- The scan in client.dll (GModDataPack, GMod 2026.11.04 builds) --------
	//
	// For one candidate name (an MSVC std::string on the stack) the game asks
	// the table for its size, then compares every entry with strcmp:
	//
	//   x86-64                                x86
	//   mov  rcx,[r15+8]     ; the table      mov  ecx,[ebx+4]
	//   mov  esi,r14d        ; i = 0          xor  esi,esi
	//   mov  rax,[rcx]                        mov  eax,[ecx]
	//   call [rax+18h]       ; GetNumStrings  call [eax+0Ch]
	//   test eax,eax                          test eax,eax
	//   jle  none                             jle  none
	// loop:
	//   ... call [rax+48h]   ; GetString(i)   ... call [eax+24h]
	//   ... call strcmp(entry, name)          ... call strcmp
	//   je   found           ; esi = i, r14 / edi = the entry
	//   ... inc esi; GetNumStrings; jl loop
	// miss:
	//   xor  r14d,r14d                        mov  edi,5C2Fh
	//
	// The patch jumps from the first instructions to a stub that asks Resolve()
	// and then continues at "found" or "miss" with the registers the scan would
	// have left, or runs the replaced instructions and the game's own loop.
	constexpr int WILD = -1;
#if defined(_WIN64)
	const int kPattern[] = {
		0x49, 0x8B, 0x4F, 0x08, 0x41, 0x8B, 0xF6, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18, 0x85, 0xC0, 0x7E, WILD,
		0x49, 0x8B, 0x4F, 0x08, 0x8B, 0xD6, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x48,
		0x48, 0x83, 0x7C, 0x24, WILD, 0x10,           // cmp qword [rsp+cap],10h  (SSO capacity)
		0x48, 0x8D, 0x54, 0x24, WILD,                 // lea rdx,[rsp+buf]
		0x48, 0x8B, 0xC8, 0x4C, 0x8B, 0xF0,
		0x48, 0x0F, 0x43, 0x54, 0x24, WILD,           // cmovae rdx,[rsp+buf]
		0xE8, WILD, WILD, WILD, WILD,                 // call strcmp
		0x85, 0xC0, 0x0F, 0x84, WILD, WILD, WILD, WILD, // je found
		0x49, 0x8B, 0x4F, 0x08, 0xFF, 0xC6, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18, 0x3B, 0xF0, 0x7C, WILD,
		0x45, 0x33, 0xF6,                             // miss: xor r14d,r14d
	};
	constexpr size_t kReplaced = 15;   // whole instructions, through "test eax,eax"
	constexpr size_t kCapAt = 33, kBufAt = 39, kBuf2At = 51, kCallAt = 52, kFoundAt = 59, kMissAt = 81;
	// The compare it calls: a plain byte-for-byte strcmp.
	const uint8_t kStrcmp[] = {
		0x48, 0x2B, 0xD1, 0x0F, 0xB6, 0x01, 0x3A, 0x04, 0x11, 0x75, 0x0A, 0x48, 0xFF, 0xC1,
		0x84, 0xC0, 0x75, 0xF1, 0x33, 0xC0, 0xC3,
	};
#elif defined(_WIN32)
	const int kPattern[] = {
		0x8B, 0x4B, 0x04, 0x33, 0xF6, 0x8B, 0x01, 0xFF, 0x50, 0x0C, 0x85, 0xC0, 0x7E, WILD,
		0x8B, 0x4B, 0x04, 0x56, 0x8B, 0x01, 0xFF, 0x50, 0x24,
		0x83, 0x7D, WILD, 0x10,                       // cmp dword [ebp+cap],10h
		0x8D, 0x4D, WILD,                             // lea ecx,[ebp+buf]
		0x8B, 0xF8,
		0x0F, 0x43, 0x4D, WILD,                       // cmovae ecx,[ebp+buf]
		0x51, 0x57, 0xE8, WILD, WILD, WILD, WILD,     // call strcmp
		0x83, 0xC4, 0x08, 0x85, 0xC0, 0x0F, 0x84, WILD, WILD, WILD, WILD, // je found
		0x8B, 0x4B, 0x04, 0x46, 0x8B, 0x01, 0xFF, 0x50, 0x0C, 0x3B, 0xF0, 0x7C, WILD,
		0xBF, 0x2F, 0x5C, 0x00, 0x00,                 // miss: mov edi,5C2Fh
	};
	constexpr size_t kReplaced = 5;    // "mov ecx,[ebx+4]" and "xor esi,esi"
	constexpr size_t kCapAt = 25, kBufAt = 29, kBuf2At = 35, kCallAt = 38, kFoundAt = 48, kMissAt = 67;
	const uint8_t kStrcmp[] = {
		0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x8B, 0x45, 0x08, 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00,
		0x8A, 0x10, 0x3A, 0x11, 0x75, 0x1A, 0x84, 0xD2, 0x74, 0x12,
	};
#endif

#ifdef _WIN32
	constexpr size_t kPatternLen = sizeof(kPattern) / sizeof(kPattern[0]);

	int32_t Rel32(const uint8_t* p)
	{
		int32_t v;
		std::memcpy(&v, p, sizeof(v));
		return v;
	}

	bool Matches(const uint8_t* p)
	{
		for (size_t i = 0; i < kPatternLen; ++i)
			if (kPattern[i] != WILD && p[i] != static_cast<uint8_t>(kPattern[i]))
				return false;
		return true;
	}

	// The patch site, decoded from its bytes.
	struct Site
	{
		size_t found = 0, miss = 0, resume = 0;   // offsets from the site
		int8_t cap = 0, buf = 0;                   // stack displacements of the name
	};

	Site Decode(const uint8_t* p)
	{
		Site s;
		s.found = kFoundAt + 6 + Rel32(p + kFoundAt + 2);
		s.miss = kMissAt;
		s.resume = kReplaced;
		s.cap = static_cast<int8_t>(p[kCapAt]);
		s.buf = static_cast<int8_t>(p[kBufAt]);
		return s;
	}

	// --- The table: public Source SDK INetworkStringTable slots ----------------
	// 3 GetNumStrings, 9 GetString, 12 FindStringIndex (the scan itself calls 3
	// and 9 through these offsets).
#ifdef _WIN64
	using GetNumStringsFn = int (*)(void*);
	using GetStringFn = const char* (*)(void*, int);
	using FindStringIndexFn = int (*)(void*, const char*);
#else
	using GetNumStringsFn = int(__thiscall*)(void*);
	using GetStringFn = const char*(__thiscall*)(void*, int);
	using FindStringIndexFn = int(__thiscall*)(void*, const char*);
#endif
	template <class Fn>
	Fn Slot(void* table, int slot)
	{
		return reinterpret_cast<Fn>((*reinterpret_cast<void***>(table))[slot]);
	}
	int GetNumStrings(void* t) { return Slot<GetNumStringsFn>(t, 3)(t); }
	const char* GetString(void* t, int i) { return Slot<GetStringFn>(t, 9)(t, i); }
	int FindStringIndex(void* t, const char* s) { return Slot<FindStringIndexFn>(t, 12)(t, s); }

	// --- State --------------------------------------------------------------------
	std::mutex g_mutex;                 // the checks below; lookups read the atomics
	uintptr_t g_engineBase = 0;
	size_t g_engineSize = 0;
	void* g_goodVtable = nullptr;       // a vtable that passed the RTTI check
	void* g_checkedTable = nullptr;     // the table whose entries were checked...
	const char* g_checkedFirst = nullptr; // ...whose first entry was stored here (a new
	                                    // table can be allocated at an old one's address)...
	int g_checkedCount = 0;             // ...up to here
	std::atomic<bool> g_refused{ false };
	std::string g_refusal;              // why, once refused (under g_mutex)

	std::atomic<long long> g_found{ 0 }, g_none{ 0 }, g_toGame{ 0 };

	uint8_t* g_site = nullptr;          // patched, or null
	uint8_t g_original[kReplaced];
	uint8_t* g_stub = nullptr;

	bool InEngine(const void* p, size_t len)
	{
		auto a = reinterpret_cast<uintptr_t>(p);
		return g_engineSize && a >= g_engineBase && a + len <= g_engineBase + g_engineSize && a + len >= a;
	}

	void Refuse(const char* why)
	{
		if (!g_refused.exchange(true))
			g_refusal = why;   // std::string assignment of a short literal; under g_mutex
	}

	// The table's class, from MSVC RTTI: the complete-object locator just before
	// the vtable names the most derived class.
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
		// signature 1; pTypeDescriptor and pSelf are image-relative
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

	// FindStringIndex must find every entry at its own index; checked once per
	// table (and again for entries added since).
	bool EntriesIndexed(void* table, int count)
	{
		const char* first = GetString(table, 0);
		if (table != g_checkedTable || first != g_checkedFirst || count < g_checkedCount)
		{
			g_checkedTable = table;
			g_checkedFirst = first;
			g_checkedCount = 0;
		}
		for (int i = g_checkedCount; i < count; ++i)
		{
			const char* s = GetString(table, i);
			if (!s || FindStringIndex(table, s) != i)
			{
				Refuse("FindStringIndex didn't find a table entry at its own index");
				return false;
			}
		}
		g_checkedCount = count;
		return true;
	}

	// --- The stub -------------------------------------------------------------------
	struct Code
	{
		std::vector<uint8_t> b;
		uintptr_t at = 0;   // where it will run

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
	};

	std::vector<uint8_t> BuildStub(const uint8_t* site, uintptr_t siteAt, uintptr_t stubAt, uintptr_t resolveAt)
	{
		const Site s = Decode(site);
		const uint8_t cap = static_cast<uint8_t>(s.cap), buf = static_cast<uint8_t>(s.buf);
		Code c;
		c.at = stubAt;
#ifdef _WIN64
		c.Put({ 0x45, 0x85, 0xF6 });                        // test r14d,r14d   (the scan starts at r14d)
		c.Put({ 0x0F, 0x85 }); size_t notZero = c.Fixup(); // jnz  game
		c.Put({ 0x49, 0x8B, 0x4F, 0x08 });                  // mov  rcx,[r15+8]        table
		c.Put({ 0x48, 0x8D, 0x54, 0x24, buf });             // lea  rdx,[rsp+buf]      name, inline
		c.Put({ 0x48, 0x83, 0x7C, 0x24, cap, 0x10 });       // cmp  qword [rsp+cap],10h
		c.Put({ 0x48, 0x0F, 0x43, 0x54, 0x24, buf });       // cmovae rdx,[rsp+buf]    name, on the heap
		c.Put({ 0x48, 0xB8 }); c.Imm<uint64_t>(resolveAt);  // mov  rax,Resolve
		c.Put({ 0xFF, 0xD0 });                              // call rax
		c.Put({ 0x83, 0xF8, 0xFE });                        // cmp  eax,-2
		c.Put({ 0x0F, 0x84 }); size_t toGame = c.Fixup();   // je   game
		c.Put({ 0x83, 0xF8, 0xFF });                        // cmp  eax,-1
		c.Put({ 0x0F, 0x84 }); size_t toMiss = c.Fixup();   // je   miss
		c.Put({ 0x8B, 0xF0 });                              // mov  esi,eax            i
		c.Put({ 0x49, 0x8B, 0x4F, 0x08 });                  // mov  rcx,[r15+8]
		c.Put({ 0x8B, 0xD6 });                              // mov  edx,esi
		c.Put({ 0x48, 0x8B, 0x01 });                        // mov  rax,[rcx]
		c.Put({ 0xFF, 0x50, 0x48 });                        // call [rax+48h]          GetString(i)
		c.Put({ 0x4C, 0x8B, 0xF0 });                        // mov  r14,rax
		c.JmpTo(siteAt + s.found);
		c.Bind(toMiss);                                     // miss:
		c.Put({ 0x48, 0xC1, 0xE8, 0x20 });                  // shr  rax,32
		c.Put({ 0x8B, 0xF0 });                              // mov  esi,eax            i = count, as the loop leaves it
		c.JmpTo(siteAt + s.miss);
		c.Bind(notZero);                                    // game:
		c.Bind(toGame);
		c.PutBytes(site, kReplaced);                        // the replaced instructions
		c.JmpTo(siteAt + s.resume);
#else
		c.Put({ 0x8B, 0x4B, 0x04 });                        // mov  ecx,[ebx+4]        table
		c.Put({ 0x8D, 0x45, buf });                         // lea  eax,[ebp+buf]      name, inline
		c.Put({ 0x83, 0x7D, cap, 0x10 });                   // cmp  dword [ebp+cap],10h
		c.Put({ 0x0F, 0x43, 0x45, buf });                   // cmovae eax,[ebp+buf]    name, on the heap
		c.Put({ 0x50 });                                    // push eax
		c.Put({ 0x51 });                                    // push ecx
		c.Put({ 0xB8 }); c.Imm<uint32_t>(static_cast<uint32_t>(resolveAt)); // mov eax,Resolve
		c.Put({ 0xFF, 0xD0 });                              // call eax                (cdecl)
		c.Put({ 0x83, 0xC4, 0x08 });                        // add  esp,8
		c.Put({ 0x83, 0xF8, 0xFE });                        // cmp  eax,-2
		c.Put({ 0x0F, 0x84 }); size_t toGame = c.Fixup();   // je   game
		c.Put({ 0x83, 0xF8, 0xFF });                        // cmp  eax,-1
		c.Put({ 0x0F, 0x84 }); size_t toMiss = c.Fixup();   // je   miss
		c.Put({ 0x8B, 0xF0 });                              // mov  esi,eax            i
		c.Put({ 0x8B, 0x4B, 0x04 });                        // mov  ecx,[ebx+4]
		c.Put({ 0x56 });                                    // push esi
		c.Put({ 0x8B, 0x01 });                              // mov  eax,[ecx]
		c.Put({ 0xFF, 0x50, 0x24 });                        // call [eax+24h]          GetString(i) (thiscall)
		c.Put({ 0x8B, 0xF8 });                              // mov  edi,eax
		c.JmpTo(siteAt + s.found);
		c.Bind(toMiss);                                     // miss:
		c.Put({ 0x8B, 0xF2 });                              // mov  esi,edx            i = count, as the loop leaves it
		c.Put({ 0x8B, 0xC2 });                              // mov  eax,edx
		c.JmpTo(siteAt + s.miss);
		c.Bind(toGame);                                     // game:
		c.PutBytes(site, kReplaced);                        // the replaced instructions
		c.JmpTo(siteAt + s.resume);
#endif
		return c.b;
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

// Called by the stub, from the middle of the game's scan.
#ifdef _WIN32
extern "C" int64_t __cdecl MeowyLuaLookupResolve(void* table, const char* name) noexcept
{
	return lua_lookup::detail::Resolve(table, name);
}
#endif

namespace lua_lookup
{
	namespace detail
	{
		int64_t Resolve(void* table, const char* name) noexcept
		{
#ifdef _WIN32
			if (!table || !name || g_refused.load(std::memory_order_relaxed))
			{
				++g_toGame;
				return kLeaveToGame;
			}
			int count = 0;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if (!IsStringTable(table))
				{
					++g_toGame;
					return kLeaveToGame;
				}
				count = GetNumStrings(table);
				if (count <= 0 || !EntriesIndexed(table, count))
				{
					++g_toGame;
					return kLeaveToGame;
				}
			}
			int index = FindStringIndex(table, name);
			if (index >= 0 && index < count)
			{
				const char* entry = GetString(table, index);
				if (!entry || std::strcmp(entry, name) != 0)
					index = -1;
			}
			else
			{
				index = -1;
			}
			++(index >= 0 ? g_found : g_none);
			return (static_cast<int64_t>(static_cast<uint32_t>(count)) << 32) | static_cast<uint32_t>(index);
#else
			(void)table;
			(void)name;
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
			(void)base;
			(void)size;
#endif
		}

		void ResetForTest()
		{
#ifdef _WIN32
			std::lock_guard<std::mutex> lock(g_mutex);
			g_goodVtable = nullptr;
			g_checkedTable = nullptr;
			g_checkedFirst = nullptr;
			g_checkedCount = 0;
			g_refused = false;
			g_refusal.clear();
			g_found = g_none = g_toGame = 0;
#endif
		}

		std::string BuildStubForTest(const uint8_t* site, uintptr_t siteAt, uintptr_t stubAt, uintptr_t resolveAt)
		{
#ifdef _WIN32
			if (!Matches(site))
				return {};
			std::vector<uint8_t> b = BuildStub(site, siteAt, stubAt, resolveAt);
			return std::string(b.begin(), b.end());
#else
			(void)site; (void)siteAt; (void)stubAt; (void)resolveAt;
			return {};
#endif
		}

		long long FindSite(const uint8_t* text, size_t size, std::string& why)
		{
#ifdef _WIN32
			long long found = -1;
			int count = 0;
			for (size_t i = 0; i + kPatternLen <= size; ++i)
			{
				if (Matches(text + i))
				{
					found = static_cast<long long>(i);
					++count;
				}
			}
			if (count != 1)
			{
				why = count == 0 ? "the client Lua lookup wasn't found (different GMod build)"
				                 : "the client Lua lookup was found more than once";
				return -1;
			}
			const uint8_t* site = text + found;
			if (site[kBufAt] != site[kBuf2At])
			{
				why = "the lookup reads the name from two places";
				return -1;
			}
			const long long callee = found + static_cast<long long>(kCallAt) + 5 + Rel32(site + kCallAt + 1);
			if (callee < 0 || static_cast<size_t>(callee) + sizeof(kStrcmp) > size ||
				std::memcmp(text + callee, kStrcmp, sizeof(kStrcmp)) != 0)
			{
				why = "the lookup's compare isn't a plain strcmp";
				return -1;
			}
			const Site s = Decode(site);
			if (static_cast<long long>(s.found) + found < 0 || found + static_cast<long long>(s.found) >= static_cast<long long>(size))
			{
				why = "the lookup's found branch leaves the code section";
				return -1;
			}
			return found;
#else
			(void)text; (void)size;
			why = "Windows only";
			return -1;
#endif
		}
	}

	bool Install(std::string& why)
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_site)
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
		const long long at = detail::FindSite(text, size, why);
		if (at < 0)
			return false;
		uint8_t* site = text + at;

		if (!g_stub)
		{
			g_stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
			if (!g_stub)
			{
				why = "couldn't allocate the stub";
				return false;
			}
		}
		const std::vector<uint8_t> stub = BuildStub(site, reinterpret_cast<uintptr_t>(site),
			reinterpret_cast<uintptr_t>(g_stub), reinterpret_cast<uintptr_t>(&MeowyLuaLookupResolve));
		DWORD old;
		if (!VirtualProtect(g_stub, 4096, PAGE_READWRITE, &old))
		{
			why = "couldn't write the stub";
			return false;
		}
		std::memcpy(g_stub, stub.data(), stub.size());
		if (!VirtualProtect(g_stub, 4096, PAGE_EXECUTE_READ, &old))
		{
			why = "couldn't make the stub executable";
			return false;
		}
		FlushInstructionCache(GetCurrentProcess(), g_stub, stub.size());

		uint8_t jump[kReplaced];
#ifdef _WIN64
		jump[0] = 0xFF; jump[1] = 0x25;                      // jmp [rip+0]
		std::memset(jump + 2, 0, 4);
		const uint64_t target = reinterpret_cast<uint64_t>(g_stub);
		std::memcpy(jump + 6, &target, 8);
		jump[14] = 0x90;                                     // nop: the 15th replaced byte
#else
		jump[0] = 0xE9;                                      // jmp rel32
		const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(g_stub) - (reinterpret_cast<uintptr_t>(site) + 5));
		std::memcpy(jump + 1, &rel, 4);
#endif
		std::memcpy(g_original, site, kReplaced);
		if (!WriteCode(site, jump, kReplaced))
		{
			why = "couldn't write the patch";
			return false;
		}
		g_site = site;
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
		if (!g_site || !WriteCode(g_site, g_original, kReplaced))
			return false;
		// The stub stays allocated: tiny, and nothing can be running in it here
		// (the main thread is the only one that runs the scan, and it's here).
		g_site = nullptr;
		return true;
#else
		return false;
#endif
	}

	void NewJoin()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		g_checkedTable = nullptr;
		g_checkedFirst = nullptr;
		g_checkedCount = 0;
#endif
	}

	bool Installed()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_site != nullptr;
#else
		return false;
#endif
	}

	std::string Describe()
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_mutex);
		std::string out = g_site ? "on" : "off";
		if (g_refused)
			out += " (left to the game: " + g_refusal + ")";
		out += ": " + std::to_string(g_found.load()) + " found, " + std::to_string(g_none.load()) + " not found, " +
			std::to_string(g_toGame.load()) + " left to the game's own scan";
		return out;
#else
		return "unsupported";
#endif
	}
}
