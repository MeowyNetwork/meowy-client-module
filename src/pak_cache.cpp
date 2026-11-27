#include "pak_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pe_image.h"
#include "thread_util.h"
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
		// Never changed once kept: a hit holds its own reference and copies it out
		// after letting go of g_mutex, so a Clear meanwhile can't free it under it.
		std::shared_ptr<const std::vector<uint8_t>> data;
	};

	using Lru = std::list<Entry>;
	using ByKey = std::unordered_map<uint64_t, Lru::iterator>;

	std::mutex g_mutex;
	Lru g_lru;                                                    // most recent first
	ByKey g_byKey;
	size_t g_held = 0;
#ifdef _WIN64
	size_t g_cap = 256u << 20;
#else
	size_t g_cap = 64u << 20;
#endif
	size_t g_perEntry = g_cap / 4;
	uint64_t g_hits = 0, g_misses = 0, g_saved = 0, g_prefetched = 0, g_waited = 0;
	std::atomic<bool> g_active{ false };
	// Entries being decompressed right now (under g_mutex): a second asker
	// waits for the first instead of doing the same work.
	std::condition_variable g_cv;
	std::unordered_set<uint64_t> g_inflight;
	// Moves on with every SetActive: workers of an earlier join stop.
	std::atomic<unsigned> g_gen{ 0 };
	std::atomic<bool> g_prefillStarted{ false };
	std::string g_prefillNote;                    // under g_mutex

	// What a clear took out of the cache, freed once g_mutex is let go.
	struct Taken
	{
		Lru lru;
		ByKey byKey;
	};

	// Takes everything out of the cache (g_mutex held): no memory is freed here.
	void TakeAllLocked(Taken& into)
	{
		into.lru.swap(g_lru);
		into.byKey.swap(g_byKey);
		g_held = 0;
	}

	// A cleared cache's memory is freed on a thread of its own when there's much
	// of it (up to 256 MB when a join ends, on the main thread's first in-game
	// frame), or here if that thread can't start.
	constexpr size_t kFreeOnThread = 1u << 20;

	void Free(std::shared_ptr<Taken> taken, size_t bytes)
	{
		if (!taken || taken->lru.empty())
			return;
#ifdef _WIN32
		if (bytes >= kFreeOnThread)
		{
			// Moved into the thread: if it can't start, the lambda (and the
			// memory with it) goes when Spawn returns, here.
			thread_util::Spawn(L"Meowy pak free", [taken = std::move(taken)]() mutable { taken.reset(); },
				thread_util::Priority::BelowNormal);
			return;
		}
#else
		(void)bytes;
#endif
		taken.reset();
	}

	// Empties the cache: taken out under g_mutex, freed after it (see Free).
	// `locked` runs under the same hold, after the cache is emptied.
	template <class F>
	void ClearAndFree(F&& locked)
	{
		std::shared_ptr<Taken> taken;
		try
		{
			taken = std::make_shared<Taken>();
		}
		catch (...)
		{
		}
		size_t bytes = 0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (taken)
			{
				bytes = g_held;
				TakeAllLocked(*taken);
			}
			else
			{
				// No memory even for the holder: freed here, under the lock, as before.
				g_lru.clear();
				g_byKey.clear();
				g_held = 0;
			}
			locked();
		}
		Free(std::move(taken), bytes);
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

	void StartPrefill(unsigned gen);

	int MEOWY_LZMA_CALL Cached(uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen, const uint8_t* props, size_t propsSize)
	{
		if (!g_active.load(std::memory_order_relaxed) || !srcLen || *srcLen < 9)
			return g_original(dest, destLen, src, srcLen, props, propsSize);
		// The map's pak is being read: decompress the rest of it ahead, on workers.
		if (!g_prefillStarted.exchange(true))
			StartPrefill(g_gen.load());
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
		bool PlausibleSizes(size_t csize, size_t usize)
		{
			// LZMA grows incompressible data by well under 1/16, plus its own few bytes.
			return csize > 9 && usize > 0 && csize - 9 <= usize + usize / 16 + 1024;
		}

		uint64_t Hash(const uint8_t* p, size_t n, uint64_t seed)
		{
			// Four chains over 32-byte strides, one 8-byte word each, so the CPU
			// runs them side by side (one chain waits on each multiply: this runs
			// on the main thread for every LZMA read, hits included). Each starts
			// from its own value, and they're folded in a fixed order, so the same
			// words in other lanes hash differently. The cache lives for one join:
			// no key is kept anywhere that a change here could break.
			constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
			uint64_t h = Mix(seed ^ (static_cast<uint64_t>(n) * kMul));
			size_t i = 0;
			if (n >= 32)
			{
				uint64_t lane[4] = { h, h ^ 0x243F6A8885A308D3ull, h ^ 0x13198A2E03707344ull, h ^ 0xA4093822299F31D0ull };
				for (; i + 32 <= n; i += 32)
				{
					uint64_t w[4];
					std::memcpy(w, p + i, 32);
					for (int k = 0; k < 4; ++k)
					{
						lane[k] = (lane[k] ^ w[k]) * kMul;
						lane[k] ^= lane[k] >> 29;
					}
				}
				h = Mix(lane[0]);
				h = Mix(h ^ lane[1]);
				h = Mix(h ^ lane[2]);
				h = Mix(h ^ lane[3]);
			}
			for (; i + 8 <= n; i += 8)
			{
				uint64_t w;
				std::memcpy(&w, p + i, 8);
				h = (h ^ w) * kMul;
				h ^= h >> 29;
			}
			uint64_t tail = 0;
			for (size_t k = 0; i < n; ++i, ++k)
				tail |= static_cast<uint64_t>(p[i]) << (8 * k);
			return Mix(h ^ tail);
		}

		// background: a prefill worker's own decompression (kept only while the
		// join that started it is still loading; nothing waits on it).
		// owned: the vector `dest` is the start of (a worker's own buffer), moved
		// into the cache instead of copied when the entry fills it exactly.
		//
		// g_mutex is held only to look up, splice and pick what to drop: a hit's
		// copy into `dest`, a new entry's copy, and freeing dropped entries all
		// happen with it let go, so the main thread never waits on g_mutex behind
		// a worker's multi-MB copy or free.
		static int DecodeImpl(Decoder original, uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen,
			const uint8_t* props, size_t propsSize, size_t srcBytes, bool background, unsigned gen,
			std::vector<uint8_t>* owned = nullptr)
		{
			if (!dest || !destLen || !src || !srcLen || !props || srcBytes == 0)
				return original(dest, destLen, src, srcLen, props, propsSize);
			const size_t destIn = *destLen, srcIn = *srcLen;
			const uint64_t key = Hash(src, srcBytes, Hash(props, propsSize, (static_cast<uint64_t>(destIn) << 32) ^ srcIn));
			const size_t headLen = srcBytes < sizeof(Entry::head) ? srcBytes : sizeof(Entry::head);
			std::unique_lock<std::mutex> lock(g_mutex);
			const size_t perEntry = g_perEntry;
			bool mine = false;
			for (;;)
			{
				auto it = g_byKey.find(key);
				if (it != g_byKey.end())
				{
					Entry& e = *it->second;
					if (e.destIn == destIn && e.srcIn == srcIn && e.srcBytes == srcBytes && e.out <= destIn &&
						std::memcmp(e.head, src, headLen) == 0)
					{
						if (background)
							return 0;   // already kept: nothing to do
						const std::shared_ptr<const std::vector<uint8_t>> data = e.data;
						const size_t out = e.out, used = e.used;
						g_lru.splice(g_lru.begin(), g_lru, it->second);
						++g_hits;
						g_saved += out;
						lock.unlock();
						std::memcpy(dest, data->data(), out);
						*destLen = out;
						*srcLen = used;
						return 0;
					}
				}
				if (!g_inflight.count(key))
				{
					g_inflight.insert(key);
					mine = true;
					break;
				}
				if (background)
					return 0;   // someone is on it already
				// A worker is decompressing this one: wait for it rather than do it twice.
				++g_waited;
				if (!g_cv.wait_for(lock, std::chrono::seconds(3), [&] { return !g_inflight.count(key); }))
					break;      // too long: decompress it here after all
			}
			if (background)
				++g_prefetched;
			else
				++g_misses;
			lock.unlock();
			const int result = original(dest, destLen, src, srcLen, props, propsSize);
			// The new entry, made before g_mutex is taken again (a waiter for this
			// key wakes once it's in). No memory for it: the game's own copy stands.
			const size_t out = *destLen;
			Lru made;
			if (result == 0 && out > 0 && out <= destIn && out <= perEntry)
			{
				try
				{
					Entry e;
					e.key = key;
					e.destIn = destIn;
					e.srcIn = srcIn;
					e.srcBytes = srcBytes;
					e.out = out;
					e.used = *srcLen;
					std::memset(e.head, 0, sizeof(e.head));
					std::memcpy(e.head, src, headLen);
					if (owned && owned->data() == dest && owned->size() == out)
						e.data = std::make_shared<const std::vector<uint8_t>>(std::move(*owned));
					else
						e.data = std::make_shared<const std::vector<uint8_t>>(dest, dest + out);
					made.push_back(std::move(e));
				}
				catch (...)
				{
					made.clear();
				}
			}
			Lru dropped;
			lock.lock();
			if (mine)
				g_inflight.erase(key);
			g_cv.notify_all();
			const bool keep = !made.empty() && !g_byKey.count(key) &&
				// A worker's entry only while its join loads, and not into the last
				// eighth of the cap: the game's own reads come first.
				!(background && (!g_active.load() || g_gen.load() != gen || g_held + out > g_cap - g_cap / 8));
			if (keep)
			{
				try
				{
					g_byKey[key] = made.begin();
					g_lru.splice(g_lru.begin(), made);
					g_held += out;
				}
				catch (...)
				{
					// No memory for the index node: not kept, nothing changed.
				}
				while (g_held > g_cap && !g_lru.empty())
				{
					g_held -= g_lru.back().out;
					g_byKey.erase(g_lru.back().key);
					dropped.splice(dropped.begin(), g_lru, std::prev(g_lru.end()));
				}
			}
			lock.unlock();
			return result;   // `made` (if not kept) and `dropped` are freed here, with g_mutex let go
		}

		int Decode(Decoder original, uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen,
			const uint8_t* props, size_t propsSize, size_t srcBytes)
		{
			return DecodeImpl(original, dest, destLen, src, srcLen, props, propsSize, srcBytes, false, 0);
		}

#ifdef _WIN32
		// A BSP's pakfile (lump 40, a zip whose offsets count from the lump) or a
		// plain zip: its LZMA entries (method 14) as where their local header is,
		// their compressed and their uncompressed size.
		bool PakEntries(const std::wstring& path, std::vector<PakEntry>& out, std::string& why)
		{
			HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, 0, nullptr);
			if (f == INVALID_HANDLE_VALUE)
			{
				why = "no map pak";
				return false;
			}
			const auto readAt = [&](uint64_t at, void* buf, DWORD n) {
				LARGE_INTEGER pos;
				pos.QuadPart = static_cast<LONGLONG>(at);
				DWORD got = 0;
				return SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, buf, n, &got, nullptr) && got == n;
			};
			LARGE_INTEGER size;
			uint8_t head[8 + 64 * 16];
			bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 22 && readAt(0, head, 4);
			uint64_t lump = 0, lumpLen = 0;
			if (ok && std::memcmp(head, "VBSP", 4) == 0)
			{
				ok = readAt(0, head, sizeof(head));
				int32_t off = 0, len = 0;
				std::memcpy(&off, head + 8 + 40 * 16, 4);
				std::memcpy(&len, head + 8 + 40 * 16 + 4, 4);
				ok = ok && off > 0 && len > 22 && static_cast<uint64_t>(off) + static_cast<uint64_t>(len) <= static_cast<uint64_t>(size.QuadPart);
				lump = static_cast<uint64_t>(off);
				lumpLen = static_cast<uint64_t>(len);
			}
			else if (ok && std::memcmp(head, "PK\x03\x04", 4) == 0)
			{
				lumpLen = static_cast<uint64_t>(size.QuadPart);
			}
			else
			{
				ok = false;
			}
			// The end-of-directory record: in the last 22 + 65535 bytes (a comment can follow it).
			std::vector<uint8_t> tail;
			uint32_t entries = 0, cdSize = 0, cdOffset = 0;
			if (ok)
			{
				const uint64_t want = std::min<uint64_t>(lumpLen, 22 + 65535);
				tail.resize(static_cast<size_t>(want));
				ok = readAt(lump + lumpLen - want, tail.data(), static_cast<DWORD>(want));
				bool found = false;
				for (size_t k = 0; ok && k + 22 <= tail.size(); ++k)
				{
					const size_t i = tail.size() - 22 - k;   // from the end back
					if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6)
					{
						uint16_t n16;
						std::memcpy(&n16, &tail[i + 10], 2);
						std::memcpy(&cdSize, &tail[i + 12], 4);
						std::memcpy(&cdOffset, &tail[i + 16], 4);
						entries = n16;
						found = true;
						break;
					}
				}
				ok = ok && found && cdSize > 0 && cdSize <= (16u << 20) && static_cast<uint64_t>(cdOffset) + cdSize <= lumpLen;
			}
			std::vector<uint8_t> cd;
			if (ok)
			{
				cd.resize(cdSize);
				ok = readAt(lump + cdOffset, cd.data(), cdSize);
			}
			CloseHandle(f);
			if (!ok)
			{
				why = "not a pak this can read";
				return false;
			}
			size_t pos = 0;
			for (uint32_t i = 0; i < entries && pos + 46 <= cd.size(); ++i)
			{
				uint32_t sig, csize, usize, local;
				uint16_t method, nameLen, extraLen, commentLen;
				std::memcpy(&sig, &cd[pos], 4);
				if (sig != 0x02014B50)
					break;
				std::memcpy(&method, &cd[pos + 10], 2);
				std::memcpy(&csize, &cd[pos + 20], 4);
				std::memcpy(&usize, &cd[pos + 24], 4);
				std::memcpy(&nameLen, &cd[pos + 28], 2);
				std::memcpy(&extraLen, &cd[pos + 30], 2);
				std::memcpy(&commentLen, &cd[pos + 32], 2);
				std::memcpy(&local, &cd[pos + 42], 4);
				if (method == 14 && csize > 9 && usize > 0 && static_cast<uint64_t>(local) + 30 + csize <= lumpLen)
					out.push_back({ lump + local, csize, usize });
				pos += 46u + nameLen + extraLen + commentLen;
			}
			return true;
		}

		size_t PrefillFile(const std::wstring& path, Decoder original, unsigned threads, unsigned gen, std::string& why)
		{
			std::vector<PakEntry> entries;
			if (!original || !PakEntries(path, entries, why))
				return 0;
			std::atomic<size_t> next{ 0 }, done{ 0 };
			std::atomic<bool> starved{ false };   // an allocation failed: every worker stops, the join goes on
			const auto work = [&]() {
				HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
				if (f == INVALID_HANDLE_VALUE)
					return;
				std::vector<uint8_t> src, dest;
				// One entry: false when it's skipped.
				const auto one = [&](const PakEntry& e) {
					uint8_t local[30];
					LARGE_INTEGER pos;
					pos.QuadPart = static_cast<LONGLONG>(e.local);
					DWORD got = 0;
					if (!SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) || !ReadFile(f, local, 30, &got, nullptr) || got != 30 ||
						local[0] != 'P' || local[1] != 'K' || local[2] != 3 || local[3] != 4)
						return false;
					uint16_t nameLen, extraLen;
					std::memcpy(&nameLen, local + 26, 2);
					std::memcpy(&extraLen, local + 28, 2);
					pos.QuadPart = static_cast<LONGLONG>(e.local + 30 + nameLen + extraLen);
					src.resize(e.csize);
					if (!SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) || !ReadFile(f, src.data(), static_cast<DWORD>(e.csize), &got, nullptr) ||
						got != e.csize)
						return false;
					// The zip-LZMA header ReadFromPack checks: version 2609h, props size 5.
					if (src[0] != 0x09 || src[1] != 0x26 || src[2] != 5 || src[3] != 0)
						return false;
					// A fresh buffer for an entry the last one was moved into the cache from.
					dest.resize(e.usize);
					size_t destLen = e.usize, srcLen = e.csize;
					DecodeImpl(original, dest.data(), &destLen, src.data() + 9, &srcLen, src.data() + 4, 5, e.csize - 9, true, gen, &dest);
					return true;
				};
				for (;;)
				{
					const size_t i = next++;
					if (i >= entries.size() || !g_active.load() || g_gen.load() != gen || starved.load())
						break;
					const PakEntry& e = entries[i];
					// Sizes the map's directory can't mean: never allocated for.
					if (e.usize > g_perEntry || !PlausibleSizes(e.csize, e.usize))
						continue;
					// An exception out of a std::thread ends the game (std::terminate):
					// running out of memory (32-bit, mid-join, is where it's
					// tightest) only stops the reading ahead.
					try
					{
						if (one(e))
							++done;
					}
					catch (...)
					{
						starved = true;
						std::vector<uint8_t>().swap(src);
						std::vector<uint8_t>().swap(dest);
					}
				}
				CloseHandle(f);
			};
			// The decoders stay at normal priority: the main thread waits up to 3 s
			// for an entry one of them is on, so a lower one would hold the game up
			// behind whatever else is running. There are only as many as spare
			// physical cores (StartPrefill), so they don't share the main thread's.
			std::vector<std::thread> pool;
			try
			{
				for (unsigned t = 0; t < threads; ++t)
					pool.emplace_back([&work]() {
						thread_util::NameThisThread(L"Meowy pak decode");
						work();
					});
			}
			catch (...)
			{
			}
			if (pool.empty())
				work();
			for (std::thread& t : pool)
				t.join();
			return done.load();
		}
#endif

		void SetCap(size_t total, size_t perEntry)
		{
			ClearAndFree([&]() {
				g_cap = total;
				g_perEntry = perEntry;
			});
		}

		void Clear()
		{
			ClearAndFree([]() {});
		}

		void SetActiveForTest(bool on) { g_active = on; }
		unsigned GenerationForTest() { return g_gen.load(); }

		Stats Get()
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			return { g_hits, g_misses, g_saved, g_held, g_byKey.size(), g_prefetched, g_waited };
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
		++g_gen;                  // an earlier join's workers stop
		g_prefillStarted = false;
		g_active = on;
		if (!on)
		{
			// The player is in game: the whole cache goes, freed off this thread.
			ClearAndFree([]() { g_cv.notify_all(); });
		}
		else
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_prefillNote.clear();
		}
	}

	std::string Describe()
	{
		const detail::Stats s = detail::Get();
		char line[256];
		std::snprintf(line, sizeof(line), "%u hits (%u waited for a worker), %u misses, %u done ahead, %.0f MB not decompressed again, %.0f MB kept (%u entries)",
			static_cast<unsigned>(s.hits), static_cast<unsigned>(s.waited), static_cast<unsigned>(s.misses), static_cast<unsigned>(s.prefetched),
			s.savedBytes / 1048576.0, s.heldBytes / 1048576.0, static_cast<unsigned>(s.entries));
		std::string state;
#ifdef _WIN32
		{
			std::lock_guard<std::mutex> lock(g_installMutex);
			state = g_site ? (g_active ? "on" : "idle") : ("off" + (g_why.empty() ? std::string() : " (" + g_why + ")"));
		}
#else
		state = "unsupported";
#endif
		std::string note;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			note = g_prefillNote;
		}
		return state + ": " + line + (note.empty() ? "" : "; " + note);
	}
}

#ifdef _WIN32
namespace
{
	// garrysmod\cache\map_pack.bsp: the map the game is loading, copied out by
	// the game itself (the read trace showed its pak read from there).
	std::wstring MapPakPath()
	{
		wchar_t buf[MAX_PATH];
		HMODULE fs = GetModuleHandleW(L"filesystem_stdio.dll");
		const DWORD n = fs ? GetModuleFileNameW(fs, buf, MAX_PATH) : 0;
		if (n == 0 || n >= MAX_PATH)
			return std::wstring();
		std::wstring dir(buf, n);
		dir = dir.substr(0, dir.find_last_of(L'\\'));                  // ...\bin or ...\bin\win64
		const auto endsWith = [&](const wchar_t* tail) {
			const size_t len = std::wcslen(tail);
			return dir.size() >= len && _wcsicmp(dir.c_str() + dir.size() - len, tail) == 0;
		};
		if (endsWith(L"\\win64"))
			dir = dir.substr(0, dir.size() - 6);
		if (!endsWith(L"\\bin"))
			return std::wstring();
		return dir.substr(0, dir.size() - 4) + L"\\garrysmod\\cache\\map_pack.bsp";
	}

	// Physical cores (a core with two hardware threads counts once), or 0 if
	// Windows can't say.
	unsigned PhysicalCores()
	{
		DWORD len = 0;
		GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
		if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0)
			return 0;
		std::vector<uint8_t> buf;
		try
		{
			buf.resize(len);
		}
		catch (...)
		{
			return 0;
		}
		auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
		if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len))
			return 0;
		unsigned cores = 0;
		for (DWORD at = 0; at < len;)
		{
			auto* one = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + at);
			if (one->Size == 0)
				break;
			if (one->Relationship == RelationProcessorCore)
				++cores;
			at += one->Size;
		}
		return cores;
	}

	void StartPrefill(unsigned gen)
	{
		const std::wstring path = MapPakPath();
		if (path.empty())
			return;
		// Two physical cores left over, for the main thread (the wall while a
		// map loads) and the material system's. Counting hardware threads would
		// put decoders on the main thread's own core (its second hardware
		// thread), slowing it down by a fifth or so while they run.
		unsigned cores = PhysicalCores();
		if (cores == 0)
		{
			const unsigned logical = std::thread::hardware_concurrency();
			cores = logical > 1 ? logical / 2 : 1;
		}
		unsigned threads = cores > 2 ? cores - 2 : 1;
#ifdef _WIN64
		threads = (std::min)(threads, 6u);
#else
		threads = (std::min)(threads, 3u);   // 32-bit: a buffer each, and the address space is tight
#endif
		{
			// The coordinator only waits for the decoders, so its own priority
			// doesn't matter (theirs are set in PrefillFile).
			const bool started = thread_util::Spawn(L"Meowy pak prefill", [path, threads, gen]() {
				const auto t0 = std::chrono::steady_clock::now();
				std::string why;
				size_t n = 0;
				try
				{
					n = pak_cache::detail::PrefillFile(path, g_original, threads, gen, why);
				}
				catch (...)
				{
					why = "out of memory";   // the map's directory, or a thread: nothing ahead this join
				}
				const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				char line[160];
				if (n || why.empty())
					std::snprintf(line, sizeof(line), "%u entries ahead on %u threads in %.1f s", static_cast<unsigned>(n), threads, secs);
				else
					std::snprintf(line, sizeof(line), "nothing ahead (%s)", why.c_str());
				std::lock_guard<std::mutex> lock(g_mutex);
				if (g_gen.load() == gen)
					g_prefillNote = line;
			});
			if (!started)
			{
				// Not tried again this join (g_prefillStarted stays set): every
				// LZMA read would try to start a thread that can't start.
				std::lock_guard<std::mutex> lock(g_mutex);
				if (g_gen.load() == gen)
					g_prefillNote = "nothing ahead (couldn't start a thread)";
			}
		}
	}
}
#endif
