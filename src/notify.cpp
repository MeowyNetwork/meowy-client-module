#include "notify.h"
#include "file_util.h"
#include "notify_catalogue.h"

#include <atomic>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	// The kinds the server's notification system sends, with the pack's own
	// words. Generic on purpose: no names, amounts or places, since toasts show
	// on the lock screen. ("payday" is left out: it happens every few minutes.)
	// The fallback when no signed catalogue (notify_catalogue.h) has been
	// accepted; kept in step with meowy_city's registry at each release.
	const notify::Kind kKinds[] = {
		{ "wanted_on_you", L"You are wanted by the police." },
		{ "wanted_cleared", L"You are no longer wanted." },
		{ "warrant_on_you", L"A warrant was issued for you." },
		{ "warrant_ended", L"Your search warrant ended." },
		{ "arrested", L"You were arrested." },
		{ "released", L"You were released from jail." },
		{ "cp_wanted", L"A new suspect is wanted." },
		{ "cp_warrant", L"A search warrant was issued." },
		{ "printer_attacked", L"Your printer is under attack." },
		{ "printer_destroyed", L"Your printer was destroyed." },
		{ "printer_robbed", L"Someone took money from your printer." },
		{ "door_lockpick", L"Someone is picking your lock." },
		{ "door_lockpicked", L"Your lock was picked." },
		{ "keypad_cracking", L"Someone is cracking your keypad." },
		{ "hit_accepted", L"You have an active contract." },
		{ "hit_completed", L"Your contract is complete." },
		{ "hit_done_customer", L"A contract you placed was completed." },
		{ "ticket_claimed", L"Staff claimed your ticket." },
		{ "ticket_reply", L"Staff replied to your ticket." },
		{ "ticket_released", L"Your ticket is back in the queue." },
		{ "ticket_closed", L"Your ticket was closed." },
		{ "server_restart", L"The server is restarting soon." },
		{ "lockdown_started", L"A lockdown started." },
		{ "lockdown_ended", L"The lockdown ended." },
		// meowy_police (dispatch, warrants, booking, fines) and the first hour.
		{ "dispatch_911", L"A new 911 call came in." },
		{ "dispatch_panic", L"An officer pressed the panic button." },
		{ "dispatch_accepted", L"An officer is on the way." },
		{ "dispatch_taken", L"Another officer took a call." },
		{ "dispatch_expired", L"No officer answered your 911 call." },
		{ "warrant_approval", L"An officer needs your approval for a warrant." },
		{ "warrant_decision", L"Your warrant request was answered." },
		{ "booked", L"You were booked by the police." },
		{ "police_fine", L"You received a police fine." },
		{ "milestone_done", L"You completed a first-hour milestone." },
		{ "guide_complete", L"You finished the first-hour guide." },
		{ "staff_newplayer_kill", L"A new player was killed. Logged for staff review." },
	};
	constexpr size_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);

	// An accepted catalogue's kinds. Built once per accepted catalogue and never
	// freed (a few per game at most), so a Kind* handed to the toast thread
	// stays valid whatever is installed after it.
	struct Snapshot
	{
		uint64_t version = 0;
		std::vector<std::string> ids;
		std::vector<std::wstring> texts;
		std::vector<notify::Kind> kinds;
	};
	std::atomic<const Snapshot*> g_active{ nullptr };
}

namespace notify
{
	const Kind* Kinds(size_t* count)
	{
		const Snapshot* s = g_active.load();
		if (s)
		{
			if (count)
				*count = s->kinds.size();
			return s->kinds.data();
		}
		if (count)
			*count = kKindCount;
		return kKinds;
	}

	int FindKind(const char* id, size_t len)
	{
		if (!id)
			return -1;
		size_t count = 0;
		const Kind* kinds = Kinds(&count);
		for (size_t i = 0; i < count; ++i)
			if (std::strlen(kinds[i].id) == len && std::memcmp(kinds[i].id, id, len) == 0)
				return static_cast<int>(i);
		return -1;
	}

	uint64_t CatalogueVersion()
	{
		const Snapshot* s = g_active.load();
		return s ? s->version : 0;
	}

	bool Install(const notify_catalogue::Catalogue& c)
	{
		if (c.version == 0 || c.kinds.empty() || c.kinds.size() > notify_catalogue::kMaxKinds)
			return false;
		if (c.version < CatalogueVersion())
			return false;
		Snapshot* s = new Snapshot();
		s->version = c.version;
		s->ids.reserve(c.kinds.size());
		s->texts.reserve(c.kinds.size());
		for (const notify_catalogue::Entry& e : c.kinds)
		{
			s->ids.push_back(e.id);
			s->texts.push_back(e.text);
		}
		// Pointers into ids/texts are taken only after both are complete, so no
		// reallocation can move them.
		s->kinds.reserve(c.kinds.size());
		for (size_t i = 0; i < s->ids.size(); ++i)
			s->kinds.push_back({ s->ids[i].c_str(), s->texts[i].c_str() });
		g_active.store(s);
		return true;
	}

	const char* Describe(Refusal r)
	{
		switch (r)
		{
		case Refusal::None: return "ok";
		case Refusal::NotOurServer: return "only works on Meowy Network servers";
		case Refusal::UnknownKind: return "unknown kind";
		case Refusal::Disabled: return "disabled";
		case Refusal::Focused: return "focused";
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

	void RateLimiter::ResetKinds()
	{
		for (int i = 0; i < kMaxKinds; ++i)
		{
			m_seenKind[i] = false;
			m_lastKind[i] = 0;
		}
	}

	static_assert(kKindCount <= 64, "RateLimiter tracks at most 64 kinds");
	static_assert(notify_catalogue::kMaxKinds <= 64, "RateLimiter tracks at most 64 kinds");

	Refusal Decide(const char* kind, size_t len, const Inputs& in, RateLimiter& limiter, int* kindIndex)
	{
		if (!in.ourServer)
			return Refusal::NotOurServer;
		int index = FindKind(kind, len);
		if (index < 0)
			return Refusal::UnknownKind;
		if (!in.enabled)
			return Refusal::Disabled;
		if (in.focused)
			return Refusal::Focused;
		if (!limiter.Take(index, in.nowMs))
			return Refusal::RateLimited;
		if (kindIndex)
			*kindIndex = index;
		return Refusal::None;
	}

#ifdef _WIN32
	bool LoadEnabled(const std::wstring& file)
	{
		HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return true;
		char buf[256];
		DWORD n = 0;
		BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
		CloseHandle(h);
		if (!ok)
			return true;
		buf[n] = '\0';
		return std::strstr(buf, "enabled=0") == nullptr;
	}

	// All at once: an interrupted write mustn't leave an empty file, which reads
	// as "on" and would bring back notifications the player turned off.
	bool SaveEnabled(const std::wstring& file, bool enabled)
	{
		return file_util::WriteAtomic(file, enabled ? "enabled=1\r\n" : "enabled=0\r\n");
	}
#else
	bool LoadEnabled(const std::wstring&) { return true; }
	bool SaveEnabled(const std::wstring&, bool) { return false; }
#endif
}
