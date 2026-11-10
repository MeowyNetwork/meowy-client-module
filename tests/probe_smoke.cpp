// Connection resilience's own A2S_INFO probe (resilience_net.cpp), no game:
//   probe_smoke                             offline: a probe thread that can't start leaves none running
//   probe_smoke <our server a.b.c.d:port>   asks it for real
// Prints whether the server answered and whether it says it has a password.
// Only our servers are asked (server_gate's list); anything else is refused.
#include "../src/resilience.h"
#include "../src/server_gate.h"
#include "../src/thread_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		// Spawns forced to fail, so nothing is sent: each one is refused, never
		// "one already on its way" (the flag stuck), and nothing is waited for.
		const std::string ours = server_gate::PrimaryAddress();
		thread_util::FailNextSpawns(2);
		const bool first = resilience::StartProbe(ours), second = resilience::StartProbe(ours);
		thread_util::FailNextSpawns(0);
		bool locked = false;
		const int result = resilience::ProbeResult(locked);
		const bool ok = !first && !second && result == 0;
		std::printf("%s: a failed probe spawn leaves no probe running\n", ok ? "ok" : "FAIL");
		return ok ? 0 : 1;
	}
	if (!resilience::StartProbe(argv[1]))
	{
		std::printf("refused: not one of our servers\n");
		return 1;
	}
	bool locked = false;
	int r = 0;
	for (int i = 0; i < 100 && r == 0; ++i)
	{
		Sleep(100);
		r = resilience::ProbeResult(locked);
	}
	std::printf("%s%s\n", r > 0 ? "answered" : "no answer", r > 0 && locked ? " (password)" : "");
	return r > 0 ? 0 : 1;
}
