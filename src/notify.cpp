#include "notify.h"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	// The kinds the server's notification system sends, with the pack's own
	// words. Generic on purpose: no names, amounts or places, since toasts show
	// on the lock screen. ("payday" is left out: it happens every few minutes.)
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
	};
	constexpr size_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);
}

namespace notify
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

	static_assert(kKindCount <= 64, "RateLimiter tracks at most 64 kinds");

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

	bool SaveEnabled(const std::wstring& file, bool enabled)
	{
		HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		const char* text = enabled ? "enabled=1\r\n" : "enabled=0\r\n";
		DWORD n = 0;
		BOOL ok = WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &n, nullptr);
		CloseHandle(h);
		return ok && n == std::strlen(text);
	}
#else
	bool LoadEnabled(const std::wstring&) { return true; }
	bool SaveEnabled(const std::wstring&, bool) { return false; }
#endif
}
