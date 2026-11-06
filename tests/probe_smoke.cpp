// Connection resilience's own A2S_INFO probe (resilience_net.cpp), no game:
//   probe_smoke <our server a.b.c.d:port>
// Prints whether the server answered and whether it says it has a password.
// Only our servers are asked (server_gate's list); anything else is refused.
#include "../src/resilience.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: probe_smoke a.b.c.d:port\n");
		return 2;
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
