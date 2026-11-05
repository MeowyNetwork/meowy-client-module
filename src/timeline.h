#pragma once
// Steam Timeline markers for Meowy Network's city events: the rules (no Steam
// or Windows calls here, so tests/timeline_smoke.cpp can run them without the
// game). timeline_steam.h does the one call into Steam.
//
// Steam's game recording (Steam settings, "Game Recording") shows a marker on
// its timeline for each event a game reports, so the player can jump straight
// to "Arrested" and clip it. The server's client Lua asks for a marker by kind
// only, from the fixed list in timeline.cpp: meowy.TimelineEvent("arrested").
// The title, description and icon are the pack's, compiled in with the kind;
// nothing a server sends reaches Steam's UI. A request is refused, in this
// order, when:
//   1. the game isn't connected to one of our servers (server_gate.h);
//   2. the kind isn't on the list;
//   3. Steam's Timeline API can't be reached (old Steam client, no steam_api);
//   4. the rate limit is reached: 1 per kind per 30 s, 60 per hour in all.
// Only a request that gets through all four counts towards the rate limit.
// Unlike notifications, it works whether or not the game is in front.

#include <cstddef>
#include <cstdint>

namespace timeline
{
	constexpr uint64_t kPerKindMs = 30 * 1000;
	constexpr uint64_t kWindowMs = 60 * 60 * 1000;
	constexpr int kPerWindow = 60;

	// Steam's own limit for an event's priority (k_unMaxTimelinePriority).
	constexpr uint32_t kMaxPriority = 1000;

	// ETimelineEventClipPriority's values (isteamtimeline.h).
	enum class Clip : int
	{
		None = 1,       // a marker only
		Standard = 2,   // Steam may suggest it as a clip
		Featured = 3,   // one of the best moments
	};

	struct Kind
	{
		const char* id;
		const char* title;         // UTF-8, shown on the marker
		const char* description;   // UTF-8, shown when it's selected
		const char* icon;          // one of Steam's built-in "steam_" icons
		uint32_t priority;         // 0..kMaxPriority: which marker wins when they overlap
		Clip clip;
	};

	// The fixed list, and how many there are.
	const Kind* Kinds(size_t* count);
	// Index into Kinds() of exactly this id (length-checked, so an embedded
	// NUL or trailing text never matches), or -1.
	int FindKind(const char* id, size_t len);

	enum class Refusal
	{
		None,
		NotOurServer,
		UnknownKind,
		Unavailable,
		RateLimited,
	};
	// The reason string Lua gets with `false`.
	const char* Describe(Refusal r);

	class RateLimiter
	{
	public:
		// Whether a marker of `kind` may go out at `nowMs` (any monotonic
		// clock); if so, it is counted.
		bool Take(int kind, uint64_t nowMs);
		void Reset();

	private:
		static constexpr int kMaxKinds = 32;
		uint64_t m_lastKind[kMaxKinds] = {};
		bool m_seenKind[kMaxKinds] = {};
		uint64_t m_recent[kPerWindow] = {};   // ring of the last kPerWindow markers
		int m_next = 0, m_count = 0;
	};

	struct Inputs
	{
		bool ourServer = false;   // server_gate::Check() == Allowed
		bool available = false;   // timeline_steam::Available()
		uint64_t nowMs = 0;
	};

	// Runs the four checks. On None, *kindIndex is the kind to send and the
	// limiter has counted it.
	Refusal Decide(const char* kind, size_t len, const Inputs& in, RateLimiter& limiter, int* kindIndex);
}
