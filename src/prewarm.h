#pragma once

// Join pre-warm (roadmap U7, first part): read a server's big files into the
// OS file cache before you join it, so a disk-cold join reads them from memory.
//
// Record: while a join loads, every file the game opens for reading through
// its file system that's 1 MB or more (the server's Workshop .gma files, the
// map, big downloads) is noted with its size (fs_cache's FS_fopen hook calls
// Opened). Once in game the list is kept for that server in
// %LOCALAPPDATA%\MeowyClientPack\joins\<address>.txt, for ours or a public
// a.b.c.d:port only (the engine's own address, as everywhere else).
//
// Warm: Start(address) reads that server's files on a background thread at
// very low I/O priority, start to end, and drops the data (the OS keeps it in
// its cache). Only files still the recorded size, only on a drive without a
// seek penalty (an SSD), at most `budget` bytes, and it stops the moment a
// join starts (Cancel) or another server is warmed.

#include <cstdint>
#include <string>
#include <vector>

namespace prewarm
{
	// fs_cache's hook, after a successful read-only open while a join loads.
	void Opened(const char* path, int64_t size);

	// A join is loading: start a fresh list (and stop any warming).
	void BeginJoin();
	// In game on `address` (checked by the caller): keep the list for it.
	// Returns how many files were kept.
	int EndJoin(const std::wstring& dir, const std::string& address);

	// Whether a list is kept for `address`, and its total size.
	bool Recorded(const std::wstring& dir, const std::string& address, uint64_t* bytes = nullptr);

	// Warm `address`'s files (see above). False, with `why`, if it can't start.
	bool Start(const std::wstring& dir, const std::string& address, uint64_t budget, std::string& why);
	void Cancel();

	// One line for the status command.
	std::string Describe();

	namespace detail
	{
		// The file name a server's list is kept under: the address with ':' as '_'.
		std::wstring FileFor(const std::wstring& dir, const std::string& address);
		// "path\tsize" lines to (path, size), skipping anything malformed.
		struct Item { std::string path; int64_t size; };
		bool Parse(const std::string& text, std::vector<Item>& out);
		std::string Format(const std::vector<Item>& items);
		// Whether the drive holding `path` has no seek penalty (an SSD): 1 yes,
		// 0 no, -1 unknown.
		int NoSeekPenalty(const std::wstring& path);
	}
}
