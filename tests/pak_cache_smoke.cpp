// The LZMA pak cache's rules (pak_cache.h) with a stand-in decoder, no game:
//   pak_cache_smoke
#include "../src/pak_cache.h"

#include <cstdio>
#include <cstring>
#include <vector>

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
static int g_calls = 0;
static int g_fail = 0;
static int MEOWY_LZMA_CALL Fake(uint8_t* dest, size_t* destLen, const uint8_t* src, size_t* srcLen, const uint8_t*, size_t)
{
	++g_calls;
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
	const int before = g_calls;
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

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
