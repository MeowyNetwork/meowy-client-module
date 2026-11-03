// Connects to the local Discord app, shows a test activity for a few
// seconds, then clears it. Usage: ipc_smoke <client id> [seconds]
#include "../src/discord_ipc.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::puts("usage: ipc_smoke <client id> [seconds]");
		return 2;
	}
	int seconds = argc > 2 ? std::atoi(argv[2]) : 10;

	discord::Start(argv[1]);
	for (int i = 0; i < 50 && std::string(discord::State()) != "ready"; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	std::printf("state: %s\n", discord::State());

	discord::SetActivity("{\"details\":\"ipc_smoke test\",\"state\":\"Meowy Network\",\"timestamps\":{\"start\":" +
		std::to_string(static_cast<long long>(std::time(nullptr))) +
		"},\"buttons\":[{\"label\":\"Meowy Network\",\"url\":\"https://meowynetwork.com/\"}]}");
	std::this_thread::sleep_for(std::chrono::seconds(seconds));
	std::printf("state after %ds: %s\n", seconds, discord::State());
	std::string err = discord::LastError();
	std::printf("last error: %s\n", err.empty() ? "(none)" : err.c_str());

	discord::Shutdown();
	std::printf("state after shutdown: %s\n", discord::State());
	return 0;
}
