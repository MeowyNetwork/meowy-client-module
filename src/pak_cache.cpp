#include "pak_cache.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pe_image.h"
#endif

namespace
{
	using pak_cache::detail::Decoder;

	struct Entry
	{
		uint64_t key;
		size_t destIn, srcIn, srcBytes;   // what it was asked with
		size_t out, used;                 // what it gave: *destLen and *srcLen after
		uint8_t head[32];                 // the first compressed bytes, checked on a hit
		std::vector<uint8_t> data;
	};

	std::mutex g_mutex;
	std::list<Entry> g_lru;                                       // most recent first
	std::unordered_map<uint64_t, std::list<Entry>::iterator> g_byKey;
	size_t g_held = 0;
#ifdef _WIN64
	size_t g_cap = 256u << 20;
#else
	size_t g_cap = 64u << 20;
#endif
	size_t g_perEntry = g_cap / 4;
	uint64_t g_hits = 0, g_misses = 0, g_saved = 0;
	std::atomic<bool> g_active{ false };

	void ClearLocked()
	{
		g_lru.clear();
		g_byKey.clear();
		g_held = 0;
	}

	uint64_t Mix(uint64_t h)
	{
		h ^= h >> 33;
		h *= 0xFF51AFD7ED558CCDull;
		h ^= h >> 33;
		h *= 0xC4CEB9FE1A85EC53ull;
		h ^= h >> 33;
		return h;
	}

#ifdef _WIN32
	// --- The patch ---------------------------------------------------------------------
	constexpr int WILD = -1;
#ifdef _WIN64
	// filesystem_stdio.dll x86-64 (GMod 2026.10.26 / 11.04), in CZipPackFile::ReadFromPack:
	//   lea  rcx, [r13+4]               ; props (after the 4-byte zip-LZMA header)
	//   mov  qword [rsp+28h], 5         ; props size
	//   mov  [rsp+20h], rcx
	//   lea  r8, [r13+9]                ; src: the entry's buffer + 9
	//   mov  rcx, [rsp+40h]             ; dest
	//   lea  r9, [rsp+48h]              ; &srcLen = the entry's whole size (the buffer's)
	//   lea  rdx, [rsp+38h]             ; &destLen
	//   call LzmaUncompress             ; <- pointed at the cache
	const int kPattern[] = {
		0x49, 0x8D, 0x4D, 0x04,
		0x48, 0xC7, 0x44, 0x24, 0x28, 0x05, 0x00, 0x00, 0x00,
		0x48, 0x89, 0x4C, 0x24, 0x20,
		0x4D, 0x8D, 0x45, 0x09,
		0x48, 0x8B, 0x4C, 0x24, 0x40,
		0x4C, 0x8D, 0x4C, 0x24, 0x48,
		0x48, 0x8D, 0x54, 0x24, 0x38,
		0xE8, WILD, WILD, WILD, WILD,
	};
#else
	// filesystem_stdio.dll x86 (same builds), the same call, __stdcall:
	//   push 5 / lea eax,[ecx+4] / push eax / lea eax,[ebp-8] / push eax /
	//   lea eax,[ecx+9] / push eax / lea eax,[ebp-4] / push eax / push [ebp+14h] / call
	const int kPattern[] = {
		0x6A, 0x05,
		0x8D, 0x41, 0x04, 0x50,
		0x8D, 0x45, 0xF8, 0x50,
		0x8D, 0x41, 0x09, 0x50,
		0x8D, 0x45, 0xFC, 0x50,
		0xFF, 0x75, 0x14,
		0xE8, WILD, WILD, WILD, WILD,
	};
#endif
	constexpr size_t kPatternLen = sizeof(kPattern) / sizeof(kPattern[0]);
	constexpr size_t kCallAt = kPatternLen - 5;
	// The header check just before: "mov eax, 2609h" (the zip-LZMA version, then props size 5).
	const uint8_t kHeaderCheck[] = { 0xB8, 0x09, 0x26, 0x00, 0x00 };
	constexpr size_t kHeaderWithin = 0x40;

	std::mutex g_installMutex;
	uint8_t* g_site = nullptr;       // the call instruction
	Decoder g_original = nullptr;
	std::string g_why;

	int MEOWY_LZMA_CALL Cached(uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen, const uint8_t* props, size_t propsSize)
	{
		if (!g_active.load(std::memory_order_relaxed) || !srcLen || *srcLen < 9)
			return g_original(dest, destLen, src, srcLen, props, propsSize);
		// src is the entry's buffer + 9 (header and props) and *srcLen the buffer's
		// size, so *srcLen - 9 bytes can be read at src (the call site's own shape).
		return pak_cache::detail::Decode(g_original, dest, destLen, src, srcLen, props, propsSize, *srcLen - 9);
	}

	bool Matches(const uint8_t* p)
	{
		for (size_t i = 0; i < kPatternLen; ++i)
			if (kPattern[i] != WILD && p[i] != static_cast<uint8_t>(kPattern[i]))
				return false;
		return true;
	}

	bool WriteCode(uint8_t* at, const void* bytes, size_t len)
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

#ifdef _WIN64
	// A "jmp [rip+0]; dq Cached" stub within 2 GB of `site`, for a call rel32 to reach.
	uint8_t* StubNear(const uint8_t* site)
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity;
		const uintptr_t base = reinterpret_cast<uintptr_t>(site) & ~(gran - 1);
		for (uintptr_t step = gran; step < (1ull << 30); step += gran)
		{
			for (int dir = -1; dir <= 1; dir += 2)
			{
				const uintptr_t at = dir < 0 ? base - step : base + step;
				void* p = VirtualAlloc(reinterpret_cast<void*>(at), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
				if (!p)
					continue;
				uint8_t* stub = static_cast<uint8_t*>(p);
				const uint8_t jmp[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
				std::memcpy(stub, jmp, sizeof(jmp));
				void* target = reinterpret_cast<void*>(&Cached);
				std::memcpy(stub + sizeof(jmp), &target, sizeof(target));
				DWORD old;
				VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old);
				FlushInstructionCache(GetCurrentProcess(), stub, 16);
				return stub;
			}
		}
		return nullptr;
	}
#endif
#endif
}

namespace pak_cache
{
	namespace detail
	{
		uint64_t Hash(const uint8_t* p, size_t n, uint64_t seed)
		{
			uint64_t h = Mix(seed ^ (static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull));
			size_t i = 0;
			for (; i + 8 <= n; i += 8)
			{
				uint64_t w;
				std::memcpy(&w, p + i, 8);
				h = (h ^ w) * 0x9E3779B97F4A7C15ull;
				h ^= h >> 29;
			}
			uint64_t tail = 0;
			for (size_t k = 0; i < n; ++i, ++k)
				tail |= static_cast<uint64_t>(p[i]) << (8 * k);
			return Mix(h ^ tail);
		}

		int Decode(Decoder original, uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen,
			const uint8_t* props, size_t propsSize, size_t srcBytes)
		{
			if (!dest || !destLen || !src || !srcLen || !props || srcBytes == 0)
				return original(dest, destLen, src, srcLen, props, propsSize);
			const size_t destIn = *destLen, srcIn = *srcLen;
			const uint64_t key = Hash(src, srcBytes, Hash(props, propsSize, (static_cast<uint64_t>(destIn) << 32) ^ srcIn));
			const size_t headLen = srcBytes < sizeof(Entry::head) ? srcBytes : sizeof(Entry::head);
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				auto it = g_byKey.find(key);
				if (it != g_byKey.end())
				{
					Entry& e = *it->second;
					if (e.destIn == destIn && e.srcIn == srcIn && e.srcBytes == srcBytes && e.out <= destIn &&
						std::memcmp(e.head, src, headLen) == 0)
					{
						std::memcpy(dest, e.data.data(), e.out);
						*destLen = e.out;
						*srcLen = e.used;
						g_lru.splice(g_lru.begin(), g_lru, it->second);
						++g_hits;
						g_saved += e.out;
						return 0;
					}
				}
				++g_misses;
			}
			const int result = original(dest, destLen, src, srcLen, props, propsSize);
			if (result != 0 || *destLen > destIn)
				return result;
			const size_t out = *destLen;
			std::lock_guard<std::mutex> lock(g_mutex);
			if (out == 0 || out > g_perEntry || g_byKey.count(key))
				return result;
			Entry e;
			e.key = key;
			e.destIn = destIn;
			e.srcIn = srcIn;
			e.srcBytes = srcBytes;
			e.out = out;
			e.used = *srcLen;
			std::memset(e.head, 0, sizeof(e.head));
			std::memcpy(e.head, src, headLen);
			try
			{
				e.data.assign(dest, dest + out);
			}
			catch (...)
			{
				return result;   // no memory for it: the game's own copy stands
			}
			g_lru.push_front(std::move(e));
			g_byKey[key] = g_lru.begin();
			g_held += out;
			while (g_held > g_cap && !g_lru.empty())
			{
				g_held -= g_lru.back().out;
				g_byKey.erase(g_lru.back().key);
				g_lru.pop_back();
			}
			return result;
		}

		void SetCap(size_t total, size_t perEntry)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_cap = total;
			g_perEntry = perEntry;
			ClearLocked();
		}

		void Clear()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			ClearLocked();
		}

		void SetActiveForTest(bool on) { g_active = on; }

		Stats Get()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			return { g_hits, g_misses, g_saved, g_held, g_byKey.size() };
		}
	}

	bool Install(std::string& why)
	{
#ifdef _WIN32
		std::lock_guard<std::mutex> lock(g_installMutex);
		if (g_site)
			return true;
		HMODULE fs = GetModuleHandleW(L"filesystem_stdio.dll");
		uint8_t* text = nullptr;
		size_t size = 0;
		if (!fs || !pe_image::TextSection(fs, text, size))
		{
			why = g_why = "filesystem_stdio.dll not found";
			return false;
		}
		uint8_t* found = nullptr;
		int count = 0;
		for (size_t i = 0; i + kPatternLen <= size; ++i)
		{
			if (Matches(text + i))
			{
				found = text + i;
				++count;
			}
		}
		if (count != 1)
		{
			why = g_why = count == 0 ? "the LZMA call wasn't found (a different GMod build)" : "the LZMA call was found more than once";
			return false;
		}
		bool header = false;
		for (size_t back = 1; back <= kHeaderWithin && found - back >= text; ++back)
			header = header || std::memcmp(found - back, kHeaderCheck, sizeof(kHeaderCheck)) == 0;
		if (!header)
		{
			why = g_why = "no zip-LZMA header check before the call";
			return false;
		}
		uint8_t* site = found + kCallAt;
		int32_t rel;
		std::memcpy(&rel, site + 1, sizeof(rel));
		const uint8_t* target = site + 5 + rel;
		if (target < text || target >= text + size)
		{
			why = g_why = "the call goes outside filesystem_stdio";
			return false;
		}
#ifdef _WIN64
		uint8_t* stub = StubNear(site);
		if (!stub)
		{
			why = g_why = "no memory near filesystem_stdio for the jump";
			return false;
		}
		const intptr_t distance = reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(site + 5);
		if (distance < INT32_MIN || distance > INT32_MAX)
		{
			why = g_why = "the jump is too far";
			return false;
		}
		const int32_t newRel = static_cast<int32_t>(distance);
#else
		const int32_t newRel = static_cast<int32_t>(reinterpret_cast<intptr_t>(&Cached) - reinterpret_cast<intptr_t>(site + 5));
#endif
		g_original = reinterpret_cast<Decoder>(const_cast<uint8_t*>(target));
		if (!WriteCode(site + 1, &newRel, sizeof(newRel)))
		{
			why = g_why = "couldn't write the patch";
			return false;
		}
		g_site = site;
		g_why.clear();
		return true;
#else
		why = "Windows only";
		return false;
#endif
	}

	void SetActive(bool on)
	{
		g_active = on;
		if (!on)
			detail::Clear();
	}

	std::string Describe()
	{
		const detail::Stats s = detail::Get();
		char line[256];
		std::snprintf(line, sizeof(line), "%u hits, %u misses, %.0f MB not decompressed again, %.0f MB kept (%u entries)",
			static_cast<unsigned>(s.hits), static_cast<unsigned>(s.misses), s.savedBytes / 1048576.0, s.heldBytes / 1048576.0,
			static_cast<unsigned>(s.entries));
		std::string state;
#ifdef _WIN32
		{
			std::lock_guard<std::mutex> lock(g_installMutex);
			state = g_site ? (g_active ? "on" : "idle") : ("off" + (g_why.empty() ? std::string() : " (" + g_why + ")"));
		}
#else
		state = "unsupported";
#endif
		return state + ": " + line;
	}
}
