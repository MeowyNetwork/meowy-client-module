// The LZMA pak cache's rules (pak_cache.h) with a stand-in decoder, no game:
//   pak_cache_smoke
#include "../src/pak_cache.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int g_count = 0, g_failures = 0;
static void Check(const char* name, bool ok)
{
	++g_count;
	if (!ok)
	{
		++g_failures;
		std::printf("FAIL %s\n", name);
	}
}

// Stands in for LzmaUncompress: the "decompressed" data is the compressed bytes
// repeated to fill dest; it reports how many source bytes it used.
static std::atomic<int> g_calls{ 0 };
static int g_fail = 0;
static std::atomic<int> g_sleepMs{ 0 };
static int MEOWY_LZMA_CALL Fake(uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen, const uint8_t*, size_t)
{
	++g_calls;
	if (g_sleepMs)
		std::this_thread::sleep_for(std::chrono::milliseconds(g_sleepMs.load()));
	if (g_fail)
		return g_fail;
	const size_t n = *srcLen - 9;
	for (size_t i = 0; i < *destLen; ++i)
		dest[i] = static_cast<uint8_t>(src[i % n] + 1);
	*srcLen = n;   // used
	return 0;
}

static int Run(std::vector<uint8_t>& src, std::vector<uint8_t>& out, size_t want, size_t* gotLen = nullptr, size_t* used = nullptr)
{
	static const uint8_t props[5] = { 0x5D, 0, 0, 1, 0 };
	out.assign(want, 0);
	size_t destLen = want, srcLen = src.size() + 9;
	const int r = pak_cache::detail::Decode(&Fake, out.data(), &destLen, src.data(), &srcLen, props, 5, src.size());
	if (gotLen)
		*gotLen = destLen;
	if (used)
		*used = srcLen;
	return r;
}

int main()
{
	using pak_cache::detail::Get;
	pak_cache::detail::SetCap(1 << 20, 1 << 18);

	std::vector<uint8_t> a(1000), b(1000), out1, out2;
	for (size_t i = 0; i < a.size(); ++i)
	{
		a[i] = static_cast<uint8_t>(i * 7);
		b[i] = static_cast<uint8_t>(i * 7 + (i == 999));
	}
	size_t len1 = 0, len2 = 0, used1 = 0, used2 = 0;
	Check("first: decoded", Run(a, out1, 4000, &len1, &used1) == 0 && g_calls == 1);
	Check("again: from the cache, not decoded", Run(a, out2, 4000, &len2, &used2) == 0 && g_calls == 1);
	Check("again: the same bytes", out1 == out2);
	Check("again: the same lengths back", len2 == len1 && used2 == used1 && used2 == 1000);
	Check("counted", Get().hits == 1 && Get().misses == 1 && Get().savedBytes == 4000);
	Run(b, out2, 4000);
	Check("one byte different: decoded", g_calls == 2);
	Run(a, out2, 3000);
	Check("a different size asked: decoded", g_calls == 3);

	g_fail = 1;
	std::vector<uint8_t> c(500, 9);
	Check("a failure passes through", Run(c, out2, 1000) == 1 && g_calls == 4);
	g_fail = 0;
	Run(c, out2, 1000);
	Check("a failure isn't kept", g_calls == 5);

	// The cap: the least recently used go first; one too big isn't kept.
	pak_cache::detail::SetCap(10000, 6000);
	std::vector<uint8_t> d(100, 1), e(100, 2), f(100, 3), out;
	Run(d, out, 4000);
	Run(e, out, 4000);
	Run(d, out, 4000);                 // d used last
	const int before = g_calls.load();
	Run(f, out, 4000);                 // 12000 > 10000: e (least recent) goes
	Run(d, out, 4000);
	Check("cap: the recent one kept", g_calls == before + 1);
	Run(e, out, 4000);
	Check("cap: the least recent one went", g_calls == before + 2);
	Check("cap: held within it", Get().heldBytes <= 10000);
	std::vector<uint8_t> big(100, 7);
	Run(big, out, 7000);
	Run(big, out, 7000);
	Check("too big for one entry: not kept", g_calls == before + 4);

	pak_cache::detail::Clear();
	Check("cleared", Get().entries == 0 && Get().heldBytes == 0);

	Check("hash: different data, different hash", pak_cache::detail::Hash(a.data(), a.size()) != pak_cache::detail::Hash(b.data(), b.size()));
	Check("hash: the seed counts", pak_cache::detail::Hash(a.data(), 10, 1) != pak_cache::detail::Hash(a.data(), 10, 2));

	// A main-thread read of an entry another thread is decompressing waits for it.
	pak_cache::detail::SetCap(1 << 20, 1 << 18);
	std::vector<uint8_t> slow(300, 5), outA, outB;
	g_sleepMs = 300;
	const int callsBefore = g_calls.load();
	const uint64_t waitedBefore = Get().waited;
	std::thread first([&]() { Run(slow, outA, 2000); });
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	Run(slow, outB, 2000);
	first.join();
	g_sleepMs = 0;
	Check("in flight: decompressed once", g_calls.load() == callsBefore + 1);
	Check("in flight: the second waited and got it", Get().waited == waitedBefore + 1 && outA == outB);

	// Ahead: a BSP whose pakfile lump is a zip with LZMA entries (as Garry's Mod
	// packs maps), decompressed by workers; the game's reads then hit.
	struct Z { std::string name; uint16_t method; std::vector<uint8_t> data; uint32_t usize; };
	std::vector<Z> zs;
	const auto lzma = [](uint8_t fill, size_t n, uint8_t version2 = 5) {
		std::vector<uint8_t> d = { 0x09, 0x26, version2, 0x00, 0x5D, 0, 0, 1, 0 };
		for (size_t i = 0; i < n; ++i)
			d.push_back(static_cast<uint8_t>(fill + i));
		return d;
	};
	zs.push_back({ "materials/a.vtf", 14, lzma(10, 200), 800 });
	zs.push_back({ "materials/b.vmt", 14, lzma(90, 50), 120 });
	zs.push_back({ "readme.txt", 0, { 'h', 'i' }, 2 });
	zs.push_back({ "materials/bad.vtf", 14, lzma(3, 40, 6), 100 });   // props size 6: not what ReadFromPack takes
	std::vector<uint8_t> zip, cd;
	const auto put16 = [](std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 255); v.push_back(x >> 8); };
	const auto put32 = [&](std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); };
	for (const Z& z : zs)
	{
		const uint32_t local = static_cast<uint32_t>(zip.size());
		put32(zip, 0x04034B50); put16(zip, 20); put16(zip, 0); put16(zip, z.method); put32(zip, 0); put32(zip, 0);
		put32(zip, static_cast<uint32_t>(z.data.size())); put32(zip, z.usize);
		put16(zip, static_cast<uint16_t>(z.name.size())); put16(zip, 0);
		zip.insert(zip.end(), z.name.begin(), z.name.end());
		zip.insert(zip.end(), z.data.begin(), z.data.end());
		put32(cd, 0x02014B50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, z.method); put32(cd, 0); put32(cd, 0);
		put32(cd, static_cast<uint32_t>(z.data.size())); put32(cd, z.usize);
		put16(cd, static_cast<uint16_t>(z.name.size())); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0);
		put32(cd, local);
		cd.insert(cd.end(), z.name.begin(), z.name.end());
	}
	const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
	zip.insert(zip.end(), cd.begin(), cd.end());
	put32(zip, 0x06054B50); put16(zip, 0); put16(zip, 0); put16(zip, static_cast<uint16_t>(zs.size())); put16(zip, static_cast<uint16_t>(zs.size()));
	put32(zip, static_cast<uint32_t>(cd.size())); put32(zip, cdOffset); put16(zip, 0);
	std::vector<uint8_t> bsp(8 + 64 * 16, 0);
	std::memcpy(bsp.data(), "VBSP", 4);
	const uint32_t lumpAt = static_cast<uint32_t>(bsp.size()) + 64, lumpLen = static_cast<uint32_t>(zip.size());
	std::memcpy(&bsp[8 + 40 * 16], &lumpAt, 4);
	std::memcpy(&bsp[8 + 40 * 16 + 4], &lumpLen, 4);
	bsp.resize(lumpAt, 0xEE);
	bsp.insert(bsp.end(), zip.begin(), zip.end());
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring path = std::wstring(tmp) + L"meowy_pak_cache_smoke.bsp";
	{
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char*>(bsp.data()), static_cast<std::streamsize>(bsp.size()));
	}
	std::vector<pak_cache::detail::PakEntry> entries;
	std::string why;
	Check("pak: its LZMA entries read from the BSP's pakfile", pak_cache::detail::PakEntries(path, entries, why) && entries.size() == 3);
	pak_cache::detail::Clear();
	pak_cache::detail::SetActiveForTest(true);
	const unsigned gen = pak_cache::detail::GenerationForTest();
	const int callsAhead = g_calls.load();
	const size_t ahead = pak_cache::detail::PrefillFile(path, &Fake, 2, gen, why);
	Check("ahead: the two good entries decompressed by the workers", ahead == 2 && g_calls.load() == callsAhead + 2 && Get().prefetched >= 2);
	// The game's own read of entry a: its buffer is the entry's bytes, src = +9, srcLen = the whole size.
	std::vector<uint8_t> entryA = zs[0].data, outGame(800);
	size_t destLen = 800, srcLen = entryA.size();
	const int callsGame = g_calls.load();
	const int r = pak_cache::detail::Decode(&Fake, outGame.data(), &destLen, entryA.data() + 9, &srcLen, entryA.data() + 4, 5, entryA.size() - 9);
	Check("ahead: the game's read is a hit", r == 0 && g_calls.load() == callsGame && destLen == 800 && outGame[0] == static_cast<uint8_t>(10 + 1));
	pak_cache::detail::SetActiveForTest(false);
	pak_cache::detail::Clear();
	const int callsOff = g_calls.load();
	Check("ahead: nothing once the join is over", pak_cache::detail::PrefillFile(path, &Fake, 2, gen, why) == 0 && g_calls.load() == callsOff);
	Check("pak: not a pak", !pak_cache::detail::PakEntries(std::wstring(tmp) + L"meowy_no_such_pak.bsp", entries, why));
	DeleteFileW(path.c_str());

	// Sizes a directory entry can't mean are never allocated for (a crafted map
	// could otherwise have every worker take the whole lump at once).
	using pak_cache::detail::PlausibleSizes;
	Check("sizes: a normal entry", PlausibleSizes(9 + 400, 800));
	Check("sizes: incompressible, a little bigger", PlausibleSizes(9 + 800 + 40, 800));
	Check("sizes: compressed far bigger than the file", !PlausibleSizes(9 + 50u * 1048576, 1048576));
	Check("sizes: no LZMA header", !PlausibleSizes(9, 800) && !PlausibleSizes(0, 800));
	Check("sizes: nothing to unpack", !PlausibleSizes(400, 0));

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
