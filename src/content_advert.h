#pragma once

// Content platform (docs/CONTENT_PLATFORM.md): a server's content advert, read
// before a join. A server that publishes one has a `meowy_content` convar with
// FCVAR_NOTIFY, so it's in the server's A2S_RULES answer:
//   "1;ws=<id>,<id>,~<id>"   (the Workshop items its join will ask for)
// This side only reads that one rule's value, on a worker thread, from public
// IPv4 servers (server_rep::IsPublicAddress); the menu Lua parses and checks it
// (content_logic.lua) and decides what to download.

#include <string>
#include <vector>

namespace content_advert
{
	constexpr const char* kRule = "meowy_content";
	constexpr size_t kMaxValue = 4096;   // bytes of the value kept; longer is no advert
	constexpr int kMaxInFlight = 4;

	// Asks "a.b.c.d:port". False when not asked: not a public address, already
	// being asked, or too many in flight.
	bool Ask(const std::string& address);

	struct Answer
	{
		std::string address;
		bool answered = false;   // the server replied to the rules query at all
		bool found = false;      // ... and had a usable `meowy_content`
		std::string value;
	};
	// The answers that arrived since the last call.
	std::vector<Answer> Take();

	// One line for a status command.
	std::string Describe();

	namespace detail
	{
		// `rule`'s value in an A2S_RULES reply ("\xFF\xFF\xFF\xFF" 'E', a 16-bit
		// count, then name\0value\0 pairs). Only printable ASCII of at most
		// kMaxValue bytes counts. False when absent, malformed or unusable.
		bool RuleValue(const std::string& reply, const char* rule, std::string& value);
	}
}
