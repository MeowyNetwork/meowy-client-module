// The player's Discord status switch (discord::SetAllowed), no game and no
// Discord needed: whether the worker runs, and menu_settings.txt as the
// client realm reads it when the menu state didn't load.
//   discord_switch_smoke
// With the Discord app running it connects for a moment with a made-up
// application id (Discord refuses the handshake; nothing is shown).
// Exit code 0 when every check passes.
#include "../src/discord_ipc.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static int g_failures = 0, g_count = 0;

static void Check(const char* name, bool ok)
{
	++g_count;
	if (!ok)
	{
		++g_failures;
		std::printf("FAIL %s\n", name);
	}
}

static bool Stopped() { return std::string(discord::State()) == "stopped"; }

int main()
{
	// menu_settings.txt: only a number that is 0 turns it off, as the menu reads it.
	Check("file: nothing there, on", !discord::OffInMenuSettings(""));
	Check("file: 0, off", discord::OffInMenuSettings("meowy_updates=1\nmeowy_discord=0\nmeowy_party=1"));
	Check("file: CRLF, off", discord::OffInMenuSettings("meowy_updates=1\r\nmeowy_discord=0\r\n"));
	Check("file: 0.0, off", discord::OffInMenuSettings("meowy_discord=0.0"));
	Check("file: 1, on", !discord::OffInMenuSettings("meowy_discord=1"));
	Check("file: 2, on", !discord::OffInMenuSettings("meowy_discord=2"));
	Check("file: not a number, the default (on)", !discord::OffInMenuSettings("meowy_discord=off"));
	Check("file: empty, the default (on)", !discord::OffInMenuSettings("meowy_discord="));
	Check("file: a space, the default (on)", !discord::OffInMenuSettings("meowy_discord= 0"));
	Check("file: the first line counts", !discord::OffInMenuSettings("meowy_discord=1\nmeowy_discord=0"));
	Check("file: another name isn't it", !discord::OffInMenuSettings("meowy_discord_x=0\nxmeowy_discord=0"));

	const std::string id = "1234567890123456789";
	const std::string activity = "{\"details\":\"discord_switch_smoke\"}";

	// On by default: a server's Start starts the worker.
	Check("on by default", discord::Allowed());
	discord::Start(id);
	Check("on: Start runs the worker", !Stopped());
	discord::SetActivity(activity);

	// Off: the worker stops at once (what Discord showed is cleared on its way out).
	auto t0 = std::chrono::steady_clock::now();
	discord::SetAllowed(false);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	Check("off: the worker stopped", Stopped());
	Check("off: within a second", ms < 1000);
	Check("off: reads off", !discord::Allowed());

	// While off, a server's calls only remember.
	discord::Start(id);
	discord::SetActivity(activity);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	Check("off: Start doesn't run the worker", Stopped());

	// Back on: the status asked for since the client realm opened comes back.
	discord::SetAllowed(true);
	Check("on again: the worker runs", !Stopped());

	// Leaving the server forgets it: on again after that starts nothing.
	discord::Shutdown();
	Check("left the server: stopped", Stopped());
	discord::SetAllowed(false);
	discord::SetAllowed(true);
	Check("left the server: on again starts nothing", Stopped());

	// Off before any server asked: nothing runs until one does and it's on.
	discord::SetAllowed(false);
	discord::Start(id);
	Check("off from the start: nothing runs", Stopped());
	discord::Shutdown();
	discord::SetAllowed(true);
	Check("off from the start, left: on starts nothing", Stopped());

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
