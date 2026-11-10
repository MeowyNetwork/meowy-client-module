// Sound devices, no game:
//   audio_watch_smoke [seconds to watch]
// Reads Windows' default output and microphones (as the PC doctor shows them)
// and starts the default-output watch; with a number, watches that long and
// prints each change (plug headphones in to see one). Exit code 0 unless the
// endpoints couldn't be read at all.
#include "../src/audio_watch.h"

#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int main(int argc, char** argv)
{
	SetConsoleOutputCP(CP_UTF8);
	const audio_watch::Devices d = audio_watch::Read();
	std::printf("read: %s\n", d.ok ? "ok" : "failed");
	std::printf("output: %s%s\n", d.output.c_str(), d.outputHandsFree ? " (hands-free)" : "");
	std::printf("input: %s%s\n", d.input.c_str(), d.inputHandsFree ? " (hands-free)" : "");
	std::printf("calls input: %s\n", d.commsInput.c_str());
	std::printf("active inputs: %d\n", d.inputs);
	// Start returns at once (the watcher registers on its own thread): give it
	// up to 5 s to settle, then ask again.
	const ULONGLONG t0 = GetTickCount64();
	bool watching = audio_watch::Start();
	const ULONGLONG startMs = GetTickCount64() - t0;
	std::printf("start returned in %llu ms\n", startMs);
	for (int i = 0; i < 500 && watching && audio_watch::Describe().compare(0, 8, "starting") == 0; ++i)
		Sleep(10);
	watching = audio_watch::Start() && startMs < 100;
	std::printf("watch: %s\n", audio_watch::Describe().c_str());
	const int seconds = argc > 1 ? std::atoi(argv[1]) : 0;
	unsigned seen = audio_watch::Changes();
	for (int i = 0; i < seconds * 10; ++i)
	{
		Sleep(100);
		if (audio_watch::Changes() != seen)
		{
			seen = audio_watch::Changes();
			std::printf("default output changed (%u): %s\n", seen, audio_watch::Read().output.c_str());
		}
	}
	return d.ok && watching ? 0 : 1;
}
