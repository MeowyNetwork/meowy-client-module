#pragma once

// A description of the player's machine, so the server can see what kind of
// hardware its players actually run (CPU-bound laptops vs desktops etc.).
// Nothing identifying: no serial numbers, user names, paths or IDs.

#include <string>

namespace hardware
{
	struct Info
	{
		std::string cpu;            // brand string, e.g. "AMD Ryzen 7 6800H with Radeon Graphics"
		unsigned threads = 0;       // logical processors
		unsigned long long ramMb = 0;
		std::string gpu;            // the adapter with the most dedicated memory
		unsigned long long vramMb = 0;
		std::string os;             // e.g. "Windows 10.0.26300"
	};

	Info Get();
}
