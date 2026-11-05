// notify_smoke: the rules behind meowy.Notify (src/notify.h), without the game
// or any window.
//   notify_smoke <empty scratch folder>
// Covers the fixed kind list and its texts, the order of the checks (gate,
// kind, switch, focus, rate limit), the rate limit itself, that refused calls
// don't use up the limit, and the player's switch on disk.
#include "../src/notify.h"
#include "../src/notify_toast.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <type_traits>

static int g_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		++g_failed;
}

// Text can't come from Lua: what the worker is handed is a kind index, nothing else.
static_assert(std::is_same<decltype(&notify_toast::Post), bool (*)(int)>::value,
	"notify_toast::Post must take only a kind index");

namespace
{
	struct Expected { const char* id; const wchar_t* text; };
	const Expected kExpected[] = {
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

	notify::Inputs Ours(uint64_t now)
	{
		notify::Inputs in;
		in.ourServer = true;
		in.enabled = true;
		in.focused = false;
		in.nowMs = now;
		return in;
	}

	notify::Refusal Ask(const char* kind, const notify::Inputs& in, notify::RateLimiter& limiter, int* index = nullptr)
	{
		int dummy = -1;
		return notify::Decide(kind, kind ? std::strlen(kind) : 0, in, limiter, index ? index : &dummy);
	}

	bool WriteText(const std::wstring& path, const char* text)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		DWORD n = 0;
		WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &n, nullptr);
		CloseHandle(h);
		return true;
	}
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2)
	{
		std::printf("usage: notify_smoke <empty scratch folder>\n");
		return 2;
	}
	const std::wstring root = argv[1];
	CreateDirectoryW(root.c_str(), nullptr);
	using notify::Refusal;

	// --- The fixed list -----------------------------------------------------------
	size_t count = 0;
	const notify::Kind* kinds = notify::Kinds(&count);
	const size_t expectedCount = sizeof(kExpected) / sizeof(kExpected[0]);
	Check(count == expectedCount, "kinds: exactly the 24 agreed kinds");
	bool same = count == expectedCount;
	for (size_t i = 0; same && i < count; ++i)
		same = std::strcmp(kinds[i].id, kExpected[i].id) == 0 && std::wcscmp(kinds[i].text, kExpected[i].text) == 0;
	Check(same, "kinds: ids and texts match the agreed list word for word");
	bool generic = true;
	for (size_t i = 0; i < count; ++i)
	{
		const wchar_t* t = kinds[i].text;
		if (std::wcslen(t) == 0 || std::wcslen(t) > 120 || std::wcsstr(t, L"http") || std::wcschr(t, L'<') || std::wcschr(t, L'&'))
			generic = false;
	}
	Check(generic, "kinds: texts are short, plain, and link-free");
	Check(notify::FindKind("payday", 6) < 0, "kinds: payday is deliberately not on the list");

	// --- Kind validation -----------------------------------------------------------
	notify::RateLimiter limiter;
	uint64_t t = 1000000;
	Check(Ask("printer_attacked", Ours(t), limiter) == Refusal::None, "kind: a listed kind is accepted");
	Check(Ask("not_a_kind", Ours(t += 20000), limiter) == Refusal::UnknownKind, "kind: an unlisted kind is refused");
	Check(std::strcmp(notify::Describe(Refusal::UnknownKind), "unknown kind") == 0, "kind: reason is \"unknown kind\"");
	Check(Ask("", Ours(t), limiter) == Refusal::UnknownKind, "kind: empty string refused");
	Check(Ask(nullptr, Ours(t), limiter) == Refusal::UnknownKind, "kind: no string (nil / a table) refused");
	Check(Ask("ARRESTED", Ours(t), limiter) == Refusal::UnknownKind, "kind: case matters");
	Check(Ask("arrest", Ours(t), limiter) == Refusal::UnknownKind, "kind: a prefix doesn't match");
	Check(Ask("arrested https://evil.example", Ours(t), limiter) == Refusal::UnknownKind, "kind: trailing text doesn't match");
	{
		const char withNul[] = "arrested\0Your bank PIN please";
		int index = -1;
		Check(notify::Decide(withNul, sizeof(withNul) - 1, Ours(t), limiter, &index) == Refusal::UnknownKind,
			"kind: an embedded NUL with text after it doesn't match (Lua strings carry their length)");
	}
	bool allFound = true;
	for (size_t i = 0; i < expectedCount; ++i)
		if (notify::FindKind(kExpected[i].id, std::strlen(kExpected[i].id)) != static_cast<int>(i))
			allFound = false;
	Check(allFound, "kind: every listed id is found at its own index");

	// --- Text never taken from Lua ------------------------------------------------------
	{
		notify::RateLimiter fresh;
		int index = -1;
		// Lua's 2nd argument isn't even read (module.cpp); whatever the kind string, the
		// shown text is the compiled-in one.
		Check(Ask("arrested", Ours(t), fresh, &index) == Refusal::None && index >= 0 &&
			kinds[index].text == notify::Kinds(nullptr)[index].text && std::wcscmp(kinds[index].text, L"You were arrested.") == 0,
			"text: the shown text is the pack's own for the kind");
	}

	// --- Gate refusal ---------------------------------------------------------------
	{
		notify::RateLimiter fresh;
		notify::Inputs other = Ours(t);
		other.ourServer = false;
		Check(Ask("arrested", other, fresh) == Refusal::NotOurServer, "gate: refused off our servers");
		Check(std::strcmp(notify::Describe(Refusal::NotOurServer), "only works on Meowy Network servers") == 0,
			"gate: reason is \"only works on Meowy Network servers\"");
		Check(Ask("not_a_kind", other, fresh) == Refusal::NotOurServer, "gate: checked before the kind (another server learns nothing)");
		other.focused = true;
		other.enabled = false;
		Check(Ask("arrested", other, fresh) == Refusal::NotOurServer, "gate: checked before everything else");
		Check(Ask("arrested", Ours(t), fresh) == Refusal::None, "gate: refusals don't use up the rate limit");
	}

	// --- The player's switch -----------------------------------------------------------
	{
		notify::RateLimiter fresh;
		notify::Inputs off = Ours(t);
		off.enabled = false;
		Check(Ask("arrested", off, fresh) == Refusal::Disabled, "switch: off refuses");
		Check(std::strcmp(notify::Describe(Refusal::Disabled), "disabled") == 0, "switch: reason is \"disabled\"");
		Check(Ask("arrested", Ours(t), fresh) == Refusal::None, "switch: refusals don't use up the rate limit");
	}

	// --- Focus refusal ----------------------------------------------------------------
	{
		notify::RateLimiter fresh;
		notify::Inputs focused = Ours(t);
		focused.focused = true;
		Check(Ask("arrested", focused, fresh) == Refusal::Focused, "focus: refused while the game is in front");
		Check(std::strcmp(notify::Describe(Refusal::Focused), "focused") == 0, "focus: reason is \"focused\"");
		Check(Ask("arrested", Ours(t), fresh) == Refusal::None, "focus: refusals don't use up the rate limit");
	}

	// --- Rate limit -------------------------------------------------------------------
	{
		notify::RateLimiter fresh;
		uint64_t now = 5000000;
		Check(Ask("printer_attacked", Ours(now), fresh) == Refusal::None, "rate: first of a kind goes out");
		Check(Ask("printer_attacked", Ours(now + 9999), fresh) == Refusal::RateLimited, "rate: same kind within 10 s refused");
		Check(std::strcmp(notify::Describe(Refusal::RateLimited), "rate limited") == 0, "rate: reason is \"rate limited\"");
		Check(Ask("printer_destroyed", Ours(now + 1), fresh) == Refusal::None, "rate: another kind isn't held up by it");
		Check(Ask("printer_attacked", Ours(now + 10000), fresh) == Refusal::None, "rate: same kind again after 10 s");
	}
	{
		// 20 in an hour, across kinds, each kind spaced past its own 10 s.
		notify::RateLimiter fresh;
		const uint64_t start = 9000000;
		int sent = 0;
		for (int i = 0; i < 20; ++i)
			if (Ask(kExpected[i % expectedCount].id, Ours(start + static_cast<uint64_t>(i) * 60000), fresh) == Refusal::None)
				++sent;
		Check(sent == 20, "rate: 20 in an hour go out");
		uint64_t at21 = start + 20ull * 60000;   // 20 minutes after the first
		Check(Ask(kExpected[21].id, Ours(at21), fresh) == Refusal::RateLimited, "rate: the 21st within the hour is refused");
		Check(Ask(kExpected[21].id, Ours(start + 3600000 - 1), fresh) == Refusal::RateLimited, "rate: still refused just before the hour");
		Check(Ask(kExpected[21].id, Ours(start + 3600000), fresh) == Refusal::None, "rate: allowed once the oldest is an hour old");
		Check(Ask(kExpected[22].id, Ours(start + 3600000 + 1), fresh) == Refusal::RateLimited, "rate: and then only as the next one ages out");
		Check(Ask(kExpected[22].id, Ours(start + 3600000 + 60000), fresh) == Refusal::None, "rate: ... which it does a minute later");
	}

	// --- The switch on disk --------------------------------------------------------------
	{
		const std::wstring file = root + L"\\notify.txt";
		DeleteFileW(file.c_str());
		Check(notify::LoadEnabled(file), "settings: on when there's no file (default on)");
		Check(notify::SaveEnabled(file, false) && !notify::LoadEnabled(file), "settings: off is kept");
		Check(notify::SaveEnabled(file, true) && notify::LoadEnabled(file), "settings: on again is kept");
		Check(WriteText(file, "garbage\r\n") && notify::LoadEnabled(file), "settings: an unreadable file means on");
		Check(!notify::SaveEnabled(root + L"\\no-such-folder\\notify.txt", false), "settings: a failed save is reported");
	}

	std::printf("%s (%d failed)\n", g_failed ? "FAILED" : "all passed", g_failed);
	return g_failed ? 1 : 0;
}
