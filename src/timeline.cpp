#include "timeline.h"

#include <cstring>

namespace
{
	// The kinds the server sends, with the pack's own words (English: the pack
	// has no translations yet). Generic on purpose, like the notifications: no
	// names, amounts or places. Icons are Steam's built-in set
	// (partner.steamgames.com/doc/features/timeline).
	const timeline::Kind kKinds[] = {
		{ "arrested", "Arrested", "You were arrested.", "steam_x", 600, timeline::Clip::Standard },
		{ "released", "Released", "You were released from jail.", "steam_checkmark", 300, timeline::Clip::None },
		{ "printer_destroyed", "Printer destroyed", "Your printer was destroyed.", "steam_explosion", 500, timeline::Clip::Standard },
		{ "hit_completed", "Contract complete", "You completed a contract.", "steam_combat", 700, timeline::Clip::Featured },
		{ "wanted_on_you", "Wanted", "You are wanted by the police.", "steam_caution", 400, timeline::Clip::Standard },
	};
	constexpr size_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);
}

namespace timeline
{
	const Kind* Kinds(size_t* count)
	{
		if (count)
			*count = kKindCount;
		return kKinds;
	}

	int FindKind(const char* id, size_t len)
	{
		if (!id)
			return -1;
		for (size_t i = 0; i < kKindCount; ++i)
			if (std::strlen(kKinds[i].id) == len && std::memcmp(kKinds[i].id, id, len) == 0)
				return static_cast<int>(i);
		return -1;
	}

	const char* Describe(Refusal r)
	{
		switch (r)
		{
		case Refusal::None: return "ok";
		case Refusal::NotOurServer: return "only works on Meowy Network servers";
		case Refusal::UnknownKind: return "unknown kind";
		case Refusal::Unavailable: return "timeline unavailable";
		case Refusal::RateLimited: return "rate limited";
		}
		return "unknown";
	}

	bool RateLimiter::Take(int kind, uint64_t nowMs)
	{
		if (kind < 0 || kind >= kMaxKinds)
			return false;
		if (m_seenKind[kind] && nowMs - m_lastKind[kind] < kPerKindMs)
			return false;
		// Full ring: the oldest of the last kPerWindow must be an hour old.
		if (m_count == kPerWindow && nowMs - m_recent[m_next] < kWindowMs)
			return false;
		m_seenKind[kind] = true;
		m_lastKind[kind] = nowMs;
		m_recent[m_next] = nowMs;
		m_next = (m_next + 1) % kPerWindow;
		if (m_count < kPerWindow)
			++m_count;
		return true;
	}

	void RateLimiter::Reset()
	{
		*this = RateLimiter();
	}

	static_assert(kKindCount <= 32, "RateLimiter tracks at most 32 kinds");

	Refusal Decide(const char* kind, size_t len, const Inputs& in, RateLimiter& limiter, int* kindIndex)
	{
		if (!in.ourServer)
			return Refusal::NotOurServer;
		int index = FindKind(kind, len);
		if (index < 0)
			return Refusal::UnknownKind;
		if (!in.available)
			return Refusal::Unavailable;
		if (!limiter.Take(index, in.nowMs))
			return Refusal::RateLimited;
		if (kindIndex)
			*kindIndex = index;
		return Refusal::None;
	}
}
