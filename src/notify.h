#pragma once
// Windows notifications for Meowy Network's city events: the rules (no
// Windows calls here, so tests/notify_smoke.cpp can run them without the game).
//
// The server's client Lua asks for a notification by kind only:
// meowy.Notify("printer_attacked"). The words the player sees are the pack's:
// the list compiled into notify.cpp, or (1.6+) a catalogue signed with our
// offline key (notify_catalogue.h). Nothing a server sends at play time is
// ever shown, so even our own server can't put a name, a link or anything
// else in a toast (they appear on the lock screen and in the notification
// centre). A request is refused, in this order, when:
//   1. the game isn't connected to one of our servers (server_gate.h);
//   2. the kind isn't on the list;
//   3. the player turned notifications off (SetNotifyEnabled);
//   4. the game's window is in front (the player can already see the game);
//   5. the rate limit is reached: 1 per kind per 10 s, 20 per hour in all.
// Only a request that gets through all five counts towards the rate limit.

#include <cstddef>
#include <cstdint>
#include <string>

namespace notify_catalogue
{
	struct Catalogue;
}

namespace notify
{
	constexpr const wchar_t* kTitle = L"Meowy Network";
	// The toast identity; the installer's Start-menu shortcut carries it.
	constexpr const wchar_t* kAppId = L"MeowyNetwork.ClientPack";
	constexpr const wchar_t* kShortcutName = L"Meowy Client Pack.lnk";

	constexpr uint64_t kPerKindMs = 10 * 1000;
	constexpr uint64_t kWindowMs = 60 * 60 * 1000;
	constexpr int kPerWindow = 20;

	struct Kind
	{
		const char* id;
		const wchar_t* text;
	};

	// The current list (the accepted catalogue's, else the compiled one), and
	// how many there are. Game thread only; the toast thread is handed the
	// Kind itself.
	const Kind* Kinds(size_t* count);
	// Index into Kinds() of exactly this id (length-checked, so an embedded
	// NUL or trailing text never matches), or -1.
	int FindKind(const char* id, size_t len);

	// The accepted catalogue's version, 0 while the compiled list applies.
	uint64_t CatalogueVersion();
	// Makes a verified, parsed catalogue the current list. Refuses one older
	// than the current catalogue (an equal version is taken again). The caller
	// checks the signature first (notify_catalogue::Verify).
	bool Install(const notify_catalogue::Catalogue& c);

	enum class Refusal
	{
		None,
		NotOurServer,
		UnknownKind,
		Disabled,
		Focused,
		RateLimited,
	};
	// The reason string Lua gets with `false`.
	const char* Describe(Refusal r);

	class RateLimiter
	{
	public:
		// Whether a notification of `kind` may go out at `nowMs` (any monotonic
		// clock); if so, it is counted.
		bool Take(int kind, uint64_t nowMs);
		void Reset();
		// Forgets the per-kind times but keeps the hourly count: after a new
		// catalogue, the indexes mean other kinds.
		void ResetKinds();

	private:
		static constexpr int kMaxKinds = 64;
		uint64_t m_lastKind[kMaxKinds] = {};
		bool m_seenKind[kMaxKinds] = {};
		uint64_t m_recent[kPerWindow] = {};   // ring of the last kPerWindow sends
		int m_next = 0, m_count = 0;
	};

	struct Inputs
	{
		bool ourServer = false;   // server_gate::Check() == Allowed
		bool enabled = true;      // the player's switch
		bool focused = false;     // the game's window is the foreground window
		uint64_t nowMs = 0;
	};

	// Runs the five checks. On None, *kindIndex is the kind to show (its text is
	// Kinds()[*kindIndex].text) and the limiter has counted it.
	Refusal Decide(const char* kind, size_t len, const Inputs& in, RateLimiter& limiter, int* kindIndex);

	// The player's switch, kept in `file` (notify.txt in %LOCALAPPDATA%\MeowyClientPack).
	// On unless the file says "enabled=0".
	bool LoadEnabled(const std::wstring& file);
	bool SaveEnabled(const std::wstring& file, bool enabled);
}
