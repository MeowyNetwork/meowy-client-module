// timeline_smoke: the rules behind meowy.TimelineEvent (src/timeline.h), without
// the game or Steam.
//   timeline_smoke
// Covers the fixed kind list (titles, descriptions, icons, priorities), the
// order of the checks (gate, kind, Steam, rate limit), the rate limit itself,
// that refused calls don't use up the limit, and that without Garry's Mod's
// steam_api in the process the Steam side says "unavailable" and loads nothing.
#include "../src/timeline.h"
#include "../src/timeline_steam.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <type_traits>

static int g_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		++g_failed;
}

// Text can't come from Lua: what reaches Steam is chosen by a kind index only.
static_assert(std::is_same<decltype(&timeline_steam::Add), bool (*)(int)>::value,
	"timeline_steam::Add must take only a kind index");

namespace
{
	struct Expected { const char* id; const char* title; };
	const Expected kExpected[] = {
		{ "arrested", "Arrested" },
		{ "released", "Released" },
		{ "printer_destroyed", "Printer destroyed" },
		{ "hit_completed", "Contract complete" },
		{ "wanted_on_you", "Wanted" },
	};
	constexpr size_t kExpectedCount = sizeof(kExpected) / sizeof(kExpected[0]);

	timeline::Inputs Ours(uint64_t now)
	{
		timeline::Inputs in;
		in.ourServer = true;
		in.available = true;
		in.nowMs = now;
		return in;
	}

	timeline::Refusal Ask(const char* kind, const timeline::Inputs& in, timeline::RateLimiter& limiter, int* index = nullptr)
	{
		int dummy = -1;
		return timeline::Decide(kind, kind ? std::strlen(kind) : 0, in, limiter, index ? index : &dummy);
	}

	bool Plain(const char* s, size_t max)
	{
		size_t n = std::strlen(s);
		return n > 0 && n <= max && !std::strstr(s, "http") && !std::strchr(s, '<') && !std::strchr(s, '&');
	}
}

int main()
{
	using timeline::Refusal;

	// --- The fixed list -----------------------------------------------------------
	size_t count = 0;
	const timeline::Kind* kinds = timeline::Kinds(&count);
	Check(count == kExpectedCount, "kinds: exactly the 5 agreed kinds");
	bool same = count == kExpectedCount;
	for (size_t i = 0; same && i < count; ++i)
		same = std::strcmp(kinds[i].id, kExpected[i].id) == 0 && std::strcmp(kinds[i].title, kExpected[i].title) == 0;
	Check(same, "kinds: ids and titles match the agreed list word for word");
	bool plain = true, icons = true, priorities = true, clips = true;
	for (size_t i = 0; i < count; ++i)
	{
		plain = plain && Plain(kinds[i].title, 40) && Plain(kinds[i].description, 120);
		icons = icons && std::strncmp(kinds[i].icon, "steam_", 6) == 0 && std::strlen(kinds[i].icon) > 6;
		priorities = priorities && kinds[i].priority <= timeline::kMaxPriority;
		int c = static_cast<int>(kinds[i].clip);
		clips = clips && c >= 1 && c <= 3;
	}
	Check(plain, "kinds: titles and descriptions are short, plain, and link-free");
	Check(icons, "kinds: every icon is one of Steam's built-in steam_ icons");
	Check(priorities, "kinds: priorities are within Steam's 0..1000");
	Check(clips, "kinds: clip priorities are valid ETimelineEventClipPriority values");

	// --- Kind validation -----------------------------------------------------------
	timeline::RateLimiter limiter;
	uint64_t t = 1000000;
	Check(Ask("arrested", Ours(t), limiter) == Refusal::None, "kind: a listed kind is accepted");
	Check(Ask("not_a_kind", Ours(t), limiter) == Refusal::UnknownKind, "kind: an unlisted kind is refused");
	Check(std::strcmp(timeline::Describe(Refusal::UnknownKind), "unknown kind") == 0, "kind: reason is \"unknown kind\"");
	Check(Ask("", Ours(t), limiter) == Refusal::UnknownKind, "kind: empty string refused");
	Check(Ask(nullptr, Ours(t), limiter) == Refusal::UnknownKind, "kind: no string (nil / a table) refused");
	Check(Ask("ARRESTED", Ours(t), limiter) == Refusal::UnknownKind, "kind: case matters");
	Check(Ask("arrest", Ours(t), limiter) == Refusal::UnknownKind, "kind: a prefix doesn't match");
	Check(Ask("printer_attacked", Ours(t), limiter) == Refusal::UnknownKind, "kind: a notification-only kind isn't a timeline kind");
	Check(Ask("released https://evil.example", Ours(t), limiter) == Refusal::UnknownKind, "kind: trailing text doesn't match");
	{
		const char withNul[] = "released\0Visit evil.example";
		Check(timeline::Decide(withNul, sizeof(withNul) - 1, Ours(t), limiter, nullptr) == Refusal::UnknownKind,
			"kind: an embedded NUL with text after it doesn't match (Lua strings carry their length)");
	}
	bool allFound = true;
	for (size_t i = 0; i < kExpectedCount; ++i)
		if (timeline::FindKind(kExpected[i].id, std::strlen(kExpected[i].id)) != static_cast<int>(i))
			allFound = false;
	Check(allFound, "kind: every listed id is found at its own index");
	{
		timeline::RateLimiter fresh;
		int index = -1;
		Check(Ask("hit_completed", Ours(t), fresh, &index) == Refusal::None && index == 3
			&& std::strcmp(kinds[index].title, "Contract complete") == 0,
			"text: what goes to Steam is the pack's own for the kind");
	}

	// --- Gate refusal ---------------------------------------------------------------
	{
		timeline::RateLimiter fresh;
		timeline::Inputs other = Ours(t);
		other.ourServer = false;
		Check(Ask("arrested", other, fresh) == Refusal::NotOurServer, "gate: refused off our servers");
		Check(std::strcmp(timeline::Describe(Refusal::NotOurServer), "only works on Meowy Network servers") == 0,
			"gate: reason is \"only works on Meowy Network servers\"");
		Check(Ask("not_a_kind", other, fresh) == Refusal::NotOurServer, "gate: checked before the kind (another server learns nothing)");
		other.available = false;
		Check(Ask("arrested", other, fresh) == Refusal::NotOurServer, "gate: checked before everything else");
		Check(Ask("arrested", Ours(t), fresh) == Refusal::None, "gate: refusals don't use up the rate limit");
	}

	// --- Steam unavailable -------------------------------------------------------------
	{
		timeline::RateLimiter fresh;
		timeline::Inputs noSteam = Ours(t);
		noSteam.available = false;
		Check(Ask("arrested", noSteam, fresh) == Refusal::Unavailable, "steam: refused when the Timeline API can't be reached");
		Check(std::strcmp(timeline::Describe(Refusal::Unavailable), "timeline unavailable") == 0,
			"steam: reason is \"timeline unavailable\"");
		Check(Ask("not_a_kind", noSteam, fresh) == Refusal::UnknownKind, "steam: the kind is checked first");
		Check(Ask("arrested", Ours(t), fresh) == Refusal::None, "steam: refusals don't use up the rate limit");
	}

	// --- Rate limit -------------------------------------------------------------------
	{
		timeline::RateLimiter fresh;
		uint64_t now = 5000000;
		Check(Ask("arrested", Ours(now), fresh) == Refusal::None, "rate: first of a kind goes out");
		Check(Ask("arrested", Ours(now + 29999), fresh) == Refusal::RateLimited, "rate: same kind within 30 s refused");
		Check(std::strcmp(timeline::Describe(Refusal::RateLimited), "rate limited") == 0, "rate: reason is \"rate limited\"");
		Check(Ask("released", Ours(now + 1), fresh) == Refusal::None, "rate: another kind isn't held up by it");
		Check(Ask("arrested", Ours(now + 30000), fresh) == Refusal::None, "rate: same kind again after 30 s");
	}
	{
		// 60 in an hour, across kinds, each kind spaced past its own 30 s.
		timeline::RateLimiter fresh;
		const uint64_t start = 9000000;
		int sent = 0;
		for (int i = 0; i < 60; ++i)
			if (Ask(kExpected[i % kExpectedCount].id, Ours(start + static_cast<uint64_t>(i) * 10000), fresh) == Refusal::None)
				++sent;
		Check(sent == 60, "rate: 60 in an hour go out");
		const uint64_t later = start + 60ull * 10000;   // 10 minutes after the first
		Check(Ask("arrested", Ours(later), fresh) == Refusal::RateLimited, "rate: the 61st within the hour is refused");
		Check(Ask("arrested", Ours(start + 3600000 - 1), fresh) == Refusal::RateLimited, "rate: still refused just before the hour");
		Check(Ask("arrested", Ours(start + 3600000), fresh) == Refusal::None, "rate: allowed once the oldest is an hour old");
		Check(Ask("released", Ours(start + 3600000 + 1), fresh) == Refusal::RateLimited, "rate: and then only as the next one ages out");
		Check(Ask("released", Ours(start + 3600000 + 10000), fresh) == Refusal::None, "rate: ... which it does 10 s later");
	}

	// --- Steam side, outside the game ----------------------------------------------------
	{
		const wchar_t* dll = sizeof(void*) == 8 ? L"steam_api64.dll" : L"steam_api.dll";
		Check(GetModuleHandleW(dll) == nullptr, "steam: no steam_api in this test process to begin with");
		Check(!timeline_steam::Available(), "steam: unavailable without Garry's Mod's steam_api");
		Check(!timeline_steam::Add(0), "steam: a marker isn't sent (and nothing crashes)");
		Check(!timeline_steam::Add(-1) && !timeline_steam::Add(1000), "steam: an out-of-range index is refused");
		Check(!timeline_steam::SetMode(timeline_steam::Mode::Playing), "steam: the game mode isn't set either");
		Check(GetModuleHandleW(dll) == nullptr, "steam: the pack never loads steam_api itself");
	}

	std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "all passed", g_failed);
	return g_failed ? 1 : 0;
}
