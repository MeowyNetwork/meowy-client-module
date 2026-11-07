#pragma once

// A map's compressed files, decompressed once per join (review 2026-11-03, P4).
//
// Most maps ship their materials and models in the BSP's pakfile compressed
// with LZMA. Garry's Mod's CZipPackFile::ReadFromPack decompresses the WHOLE
// entry on every read and keeps nothing (garrysmod-issues #6239), and a
// texture is read several times while a map loads (its header for precache,
// then its data). Measured 2026-11-07 on a first join to the dev server: the
// LZMA decoder was over half of the main thread's samples in the map step.
//
// The patch: the one call from ReadFromPack to LzmaLib's LzmaUncompress is
// pointed at Decode, which keeps each decompressed entry (keyed by a hash of
// its compressed bytes and its sizes) and hands a copy back the next time.
// Only while a join loads (SetActive), within a memory cap (256 MB x64,
// 64 MB x86; one entry at most a quarter of it), least recently used out
// first, and emptied when the join ends. Outside a join it passes straight
// through. The call site is found by its exact instructions (both builds);
// anything else and nothing is changed.
//
// Ahead: on a join's first LZMA read, worker threads read the map's pak
// (garrysmod\cache\map_pack.bsp, which the game writes as the map loads) and
// decompress its entries into the cache, so the main thread finds them done; it
// waits for one a worker is on rather than decompress it again.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pak_cache
{
	// Points the call at the cache. Main thread; filesystem_stdio loaded.
	bool Install(std::string& why);
	// On while a join loads; off empties the cache.
	void SetActive(bool on);
	// "on: 812 hits, 140 misses, 1.9 GB not decompressed again, 180 MB kept" and the like.
	std::string Describe();

	namespace detail
	{
#ifdef _WIN64
#define MEOWY_LZMA_CALL
#else
#define MEOWY_LZMA_CALL __stdcall
#endif
		// LzmaLib's LzmaUncompress (__stdcall on x86): 0 on success.
		using Decoder = int(MEOWY_LZMA_CALL*)(uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen,
			const uint8_t* props, size_t propsSize);

		// The cache in front of `original`. `srcBytes` is how many bytes at `src`
		// may be read (the hash covers them).
		int Decode(Decoder original, uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen,
			const uint8_t* props, size_t propsSize, size_t srcBytes);
		void SetCap(size_t total, size_t perEntry);
		void Clear();
		void SetActiveForTest(bool on);
		unsigned GenerationForTest();
		struct Stats { uint64_t hits, misses; uint64_t savedBytes; size_t heldBytes, entries; uint64_t prefetched, waited; };
		Stats Get();

		// Ahead of the game (the map's pak, garrysmod\cache\map_pack.bsp): its
		// LZMA entries, read from the zip directory of a BSP's pakfile lump (or a
		// plain zip), decompressed by `threads` workers into the cache while join
		// `gen` is still loading. A main-thread read of an entry a worker is on
		// waits for it (3 s at most) instead of doing it again. Returns how many
		// entries the workers decompressed.
		struct PakEntry { uint64_t local; size_t csize, usize; };
		bool PakEntries(const std::wstring& path, std::vector<PakEntry>& out, std::string& why);
		size_t PrefillFile(const std::wstring& path, Decoder original, unsigned threads, unsigned gen, std::string& why);
		uint64_t Hash(const uint8_t* p, size_t n, uint64_t seed = 0);
	}
}
