#pragma once

// A Steam Web API ticket for Meowy Network (ISteamUser::GetAuthTicketForWebApi),
// sent with each server-list report so the backend can ask Steam whose account
// it is (ISteamUserAuth/AuthenticateUserTicket) and count one vote per account
// (the Website repo's backend/src/serverRep/steamTicket.js). Through the game's
// own steam_api, as party_steam does; nothing else uses the ticket.
//
// Steam answers asynchronously, with a GetTicketForWebApiResponse_t callback
// run by SteamAPI_RunCallbacks on the game's main thread. Request and Pump are
// for that thread; Wait is for the report's worker thread.

#include <cstddef>
#include <cstdint>
#include <string>

namespace steam_ticket
{
	// The identity the ticket is made for; the backend passes the same one, so a
	// ticket made for another service isn't taken, nor ours by another.
	constexpr const char* kIdentity = "meowynetwork";
	constexpr int kMaxTicket = 2560;   // GetTicketForWebApiResponse_t's buffer

	// Game thread. Asks Steam for a ticket: its handle, or 0 when Steam can't
	// (no steam_api, its calls missing, or Steam refused). `why` says which.
	uint32_t Request(std::string* why = nullptr);

	// Any thread. The ticket as lowercase hex once Steam answered, waiting up to
	// `ms`; "" on a timeout or a refusal (`why` says which).
	std::string Wait(uint32_t handle, int ms, std::string* why = nullptr);

	// Any thread: the backend has the ticket, so it can go. Steam's
	// CancelAuthTicket runs at the next Pump (game thread).
	void Release(uint32_t handle);

	// Game thread: cancels the tickets released since the last call.
	void Pump();

	namespace detail
	{
		std::string Hex(const uint8_t* bytes, size_t n);
		// The report's JSON object with "ticket":"<hex>" added before its closing
		// brace; unchanged when it isn't an object or the hex isn't plain hex.
		std::string WithTicket(const std::string& json, const std::string& hex);
		// What the callback does with Steam's answer (tests feed it directly).
		void Arrived(uint32_t handle, int result, const uint8_t* ticket, int size);
		// A handle Request would hand out, without Steam (tests).
		void Expect(uint32_t handle);
		// Handles released and not cancelled yet (tests).
		size_t PendingCancels();
	}
}
