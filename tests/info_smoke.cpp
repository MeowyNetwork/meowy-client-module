// Prints the hardware profile and, given the GarrysMod folder, the newest
// crash reports' fault lines, without the game.
// Usage: info_smoke [path to the GarrysMod folder]
#include "../src/crash_reports.h"
#include "../src/hardware.h"

#include <cstdio>
#include <string>

static int MaskChecks()
{
	struct Case { const char* in; const char* want; };
	const Case cases[] = {
		{ "C:\\Users\\Jane Doe\\gmod\\x.lua:12: attempt", "<path>:12: attempt" },
		{ "open 'D:/Steam Library/steamapps/common/GarrysMod/garrysmod/data/a.txt' failed", "open '<path>' failed" },
		{ "C:\\Program Files (x86)\\Steam\\x.lua: bad argument", "<path>: bad argument" },
		{ "\\\\nas\\share\\Jane\\x.lua:3:", "<path>:3:" },
		{ "line one C:\\a b\\c\nline two", "line one <path>\nline two" },
		{ "addons/foo/lua/x.lua:12: attempt to index nil", "addons/foo/lua/x.lua:12: attempt to index nil" },
		{ "time 12:30 abc:\\x", "time 12:30 abc:\\x" },
	};
	int failed = 0;
	for (const Case& c : cases)
	{
		std::string got = crashes::MaskPaths(c.in);
		bool ok = got == c.want;
		std::printf("%s mask: %s -> %s\n", ok ? "ok  " : "FAIL", c.in, got.c_str());
		failed += ok ? 0 : 1;
	}
	return failed;
}

int main(int argc, char** argv)
{
	if (int failed = MaskChecks())
		return failed;
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
