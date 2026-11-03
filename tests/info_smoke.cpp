// Prints the hardware profile and, given the GarrysMod folder, the newest
// crash reports' fault lines, without the game.
// Usage: info_smoke [path to the GarrysMod folder]
#include "../src/crash_reports.h"
#include "../src/hardware.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
	hardware::Info hw = hardware::Get();
	std::printf("cpu: %s\nthreads: %u\nram: %llu MB\ngpu: %s\nvram: %llu MB\nos: %s\n",
		hw.cpu.c_str(), hw.threads, hw.ramMb, hw.gpu.c_str(), hw.vramMb, hw.os.c_str());

	if (argc > 1)
	{
		std::string a(argv[1]);
		std::wstring root(a.begin(), a.end());
		for (const crashes::Report& r : crashes::SinceIn(root, 0, 6))
			std::printf("crash %lld: fault=%s  lua=%zu bytes\n", r.time, r.fault.c_str(), r.luaTrace.size());
	}
	return 0;
}
