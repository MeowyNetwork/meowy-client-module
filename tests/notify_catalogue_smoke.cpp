// The signed notification catalogue (notify_catalogue.h, notify::Install): the
// strict parser, the ECDSA check through Windows' BCrypt, and the switch of
// the kind list, with no game.
//
// The fixture is the real catalogue signed by DarkRP's
// tools/notify_catalogue/build.py (version 202611071234), so this also proves
// the Python signer and the pack's verifier agree on the bytes.
//
//   notify_catalogue_smoke

#include "../src/notify.h"
#include "../src/notify_catalogue.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	int g_failures = 0, g_checks = 0;

	void Check(const char* label, bool ok)
	{
		++g_checks;
		if (!ok)
			++g_failures;
		std::printf("  %s %s\n", ok ? "ok  " : "FAIL", label);
	}

	const char kBody[] =
		"meowy-notify-catalogue 1\n"
		"version 202611071234\n"
		"kind arrested You were arrested.\n"
		"kind booked You were booked by the police.\n"
		"kind cp_wanted A new suspect is wanted.\n"
		"kind cp_warrant A search warrant was issued.\n"
		"kind dispatch_911 A new 911 call came in.\n"
		"kind dispatch_accepted An officer is on the way.\n"
		"kind dispatch_expired No officer answered your 911 call.\n"
		"kind dispatch_panic An officer pressed the panic button.\n"
		"kind dispatch_taken Another officer took a call.\n"
		"kind door_lockpick Someone is picking your lock.\n"
		"kind door_lockpicked Your lock was picked.\n"
		"kind guide_complete You finished the first-hour guide.\n"
		"kind hit_accepted You have an active contract.\n"
		"kind hit_completed Your contract is complete.\n"
		"kind hit_done_customer A contract you placed was completed.\n"
		"kind keypad_cracking Someone is cracking your keypad.\n"
		"kind lockdown_ended The lockdown ended.\n"
		"kind lockdown_started A lockdown started.\n"
		"kind milestone_done You completed a first-hour milestone.\n"
		"kind police_fine You received a police fine.\n"
		"kind printer_attacked Your printer is under attack.\n"
		"kind printer_destroyed Your printer was destroyed.\n"
		"kind printer_robbed Someone took money from your printer.\n"
		"kind released You were released from jail.\n"
		"kind server_restart The server is restarting soon.\n"
		"kind staff_newplayer_kill A new player was killed. Logged for staff review.\n"
		"kind ticket_claimed Staff claimed your ticket.\n"
		"kind ticket_closed Your ticket was closed.\n"
		"kind ticket_released Your ticket is back in the queue.\n"
		"kind ticket_reply Staff replied to your ticket.\n"
		"kind wanted_cleared You are no longer wanted.\n"
		"kind wanted_on_you You are wanted by the police.\n"
		"kind warrant_approval An officer needs your approval for a warrant.\n"
		"kind warrant_decision Your warrant request was answered.\n"
		"kind warrant_ended Your search warrant ended.\n"
		"kind warrant_on_you A warrant was issued for you.\n";
	const char kSig[] =
		"9d6be94f136a1b312784a8563b431551d69ccc2dbffb5ddffbc756fa100f1ed5"
		"0abb3fe2d0eb42525aa5703fe3e56e9bed59d417ac2ef96e18b21bc5557c04dc";

	bool Parses(const std::string& body, const char** why = nullptr)
	{
		notify_catalogue::Catalogue c;
		return notify_catalogue::Parse(body, &c, why);
	}

	std::string Replace(std::string s, const std::string& from, const std::string& to)
	{
		size_t at = s.find(from);
		if (at != std::string::npos)
			s.replace(at, from.size(), to);
		return s;
	}
}

int main()
{
	const std::string body(kBody), sig(kSig);

	// Signature.
	Check("our signed catalogue verifies", notify_catalogue::Verify(body, sig));
	std::string upper = sig;
	for (char& c : upper)
		if (c >= 'a' && c <= 'f')
			c = static_cast<char>(c - 'a' + 'A');
	Check("upper-case hex verifies too", notify_catalogue::Verify(body, upper));
	Check("one changed byte fails", !notify_catalogue::Verify(Replace(body, "arrested.", "arrested!"), sig));
	Check("a changed version fails", !notify_catalogue::Verify(Replace(body, "202611071234", "202611071235"), sig));
	std::string flipped = sig;
	flipped[10] = flipped[10] == '0' ? '1' : '0';
	Check("a changed signature fails", !notify_catalogue::Verify(body, flipped));
	Check("a short signature fails", !notify_catalogue::Verify(body, sig.substr(0, 126)));
	Check("non-hex signature fails", !notify_catalogue::Verify(body, std::string(127, 'a') + "z"));
	uint8_t otherKey[64] = {};
	otherKey[0] = 1;
	Check("another key fails", !notify_catalogue::VerifyWithKey(body, sig, otherKey));

	// Parser.
	notify_catalogue::Catalogue c;
	const char* why = nullptr;
	Check("our catalogue parses", notify_catalogue::Parse(body, &c, &why));
	Check("36 kinds, version 202611071234", c.kinds.size() == 36 && c.version == 202611071234ULL);
	Check("text is decoded", c.kinds.size() > 0 && c.kinds[0].id == "arrested" && c.kinds[0].text == L"You were arrested.");
	Check("refuses a wrong header", !Parses(Replace(body, "catalogue 1", "catalogue 2")));
	Check("refuses no trailing newline", !Parses(body.substr(0, body.size() - 1)));
	Check("refuses CR line ends", !Parses(Replace(body, "\n", "\r\n")));
	Check("refuses an upper-case id", !Parses(Replace(body, "kind arrested", "kind Arrested")));
	Check("refuses a kind without text", !Parses(Replace(body, "kind booked You were booked by the police.", "kind booked")));
	Check("refuses a duplicate kind", !Parses(Replace(body, "kind booked", "kind arrested")));
	Check("refuses a leading-zero version", !Parses(Replace(body, "version 2026", "version 02026")));
	Check("refuses version 0", !Parses(Replace(body, "version 202611071234", "version 0")));
	Check("refuses a control character", !Parses(Replace(body, "You were arrested.", "You were\x01 arrested.")));
	Check("refuses invalid UTF-8", !Parses(Replace(body, "You were arrested.", "You were \xC3\x28 arrested.")));
	Check("refuses an overlong UTF-8 slash", !Parses(Replace(body, "You were arrested.", "You \xC0\xAF arrested.")));
	Check("refuses a NUL", !Parses(Replace(body, "You were arrested.", std::string("You were\0arrested.", 18))));
	Check("refuses a header with no kinds", !Parses("meowy-notify-catalogue 1\nversion 5\n"));
	Check("accepts valid UTF-8 text", Parses("meowy-notify-catalogue 1\nversion 5\nkind cafe Caf\xC3\xA9 is open \xE2\x80\x94 come in.\n"));
	std::string tooMany = "meowy-notify-catalogue 1\nversion 5\n";
	for (int i = 0; i < 65; ++i)
		tooMany += "kind k" + std::to_string(i) + " Text.\n";
	Check("refuses 65 kinds", !Parses(tooMany));
	Check("refuses text over 160 bytes", !Parses("meowy-notify-catalogue 1\nversion 5\nkind a " + std::string(161, 'x') + "\n"));
	Check("refuses an id over 32 characters", !Parses("meowy-notify-catalogue 1\nversion 5\nkind " + std::string(33, 'a') + " Text.\n"));

	// The kind list.
	Check("compiled list knows dispatch_911 already", notify::FindKind("dispatch_911", 12) >= 0);
	Check("compiled list doesn't know a made-up kind", notify::FindKind("fish_rare_catch", 15) < 0);
	Check("no catalogue yet: version 0", notify::CatalogueVersion() == 0);

	notify_catalogue::Catalogue next;
	notify_catalogue::Parse("meowy-notify-catalogue 1\nversion 202611080000\n"
		"kind fish_rare_catch A rare fish was caught.\nkind arrested You were arrested.\n", &next, nullptr);
	Check("a newer catalogue installs", notify::Install(next));
	size_t count = 0;
	const notify::Kind* kinds = notify::Kinds(&count);
	Check("its kinds are the list now", count == 2 && notify::FindKind("fish_rare_catch", 15) >= 0 &&
		notify::FindKind("dispatch_911", 12) < 0);
	Check("text comes from the catalogue", count == 2 && std::wcscmp(kinds[0].text, L"A rare fish was caught.") == 0);
	Check("version is reported", notify::CatalogueVersion() == 202611080000ULL);

	Check("an older catalogue is refused", !notify::Install(c));
	Check("the list stays", notify::FindKind("fish_rare_catch", 15) >= 0);
	Check("the same version is taken again", notify::Install(next));

	// A Kind handed out earlier stays valid after a newer install.
	const notify::Kind* before = &notify::Kinds(&count)[0];
	notify_catalogue::Catalogue newer = next;
	newer.version = 202611090000ULL;
	newer.kinds[0].text = L"Changed.";
	Check("a newer catalogue replaces it", notify::Install(newer));
	Check("an earlier Kind still reads its own text", std::wcscmp(before->text, L"A rare fish was caught.") == 0);

	notify::RateLimiter limiter;
	Check("rate limiter: first is taken", limiter.Take(0, 1000));
	Check("rate limiter: repeat within 10 s refused", !limiter.Take(0, 2000));
	limiter.ResetKinds();
	Check("ResetKinds forgets the per-kind time", limiter.Take(0, 3000));

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
