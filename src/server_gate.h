#pragma once

// Which server is this game connected to, asked of the engine itself.
//
// Everything the pack offers (Discord status, crash reports, the hardware
// profile, game patches) is for Meowy Network only. A check in Lua would be
// worthless: the server controls the client's whole Lua environment and can
// replace game.GetIPAddress, the hostname or anything else before calling us.
// So the answer comes from the engine's own network channel
// (IVEngineClient::GetNetChannelInfo()->GetAddress()), which no server can
// change, and is compared with an allow list compiled into this DLL.
//
// It fails closed: not connected, singleplayer, an engine that doesn't look
// as expected, or an address that doesn't parse all count as "not allowed".

#include <string>

namespace server_gate
{
	enum class Result
	{
		Allowed,        // connected to one of our servers
		NotConnected,   // main menu, or still connecting
		OtherServer,    // connected somewhere else, including singleplayer ("loopback")
		Unavailable,    // couldn't ask the engine safely
	};

	// `address` receives what the engine reported, when there was something.
	Result Check(std::string* address = nullptr);

	const char* Describe(Result r);

	// Whether "a.b.c.d:port" is one of our servers (the same list as Check).
	bool IsOurAddress(const std::string& address);

	// Our main server's "a.b.c.d:port" (the first of that list), for the main
	// menu's status card.
	std::string PrimaryAddress();

	// The engine's own view of the connection's health: INetChannelInfo::
	// IsTimingOut (no packet for a few seconds) and GetTimeSinceLastReceived
	// (seconds). False when not connected or it couldn't be read safely.
	// Connection resilience uses it to tell a real drop (the server went
	// silent) from a kick, ban, quit or map change (packets still flowing).
	bool NetHealth(bool& timingOut, double& silence);

	// The connection as net_graph sees it, for the Escape menu's server card:
	// average round trip (seconds), incoming packet loss (0..1) and time
	// connected (seconds). False when not connected or it couldn't be read safely.
	bool NetStats(double& latency, double& loss, double& connected);
}
