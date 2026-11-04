#pragma once
// Knows which server a join is going to the moment it starts (Windows).
//
// The engine's connection only exists once the server has answered, which is
// a round trip (about 0.2 s) after the join starts; until then GMod's own
// loading page is on screen. But the engine prints "Connecting to <address>..."
// as the join starts. This registers with the engine's console log
// (tier0's LoggingSystem_RegisterLoggingListener) and remembers the last
// address it printed.
//
// It's only an early hint for showing our loading screen sooner: nothing that
// involves the player's data is decided on it. That still waits for
// server_gate, which asks the real connection.

#include <string>

namespace connect_watch
{
	// Registers once; safe to call again.
	void Start();
	// The last "Connecting to ..." line, within `withinMs`, was for one of our servers.
	bool HeadingToUs(unsigned withinMs);
	// For the join log: whether the listener is registered, lines seen, and the last
	// "Connecting to" address.
	std::string Describe();
	// The last "Connecting to" address ("" if none yet).
	std::string LastAddress();
}
