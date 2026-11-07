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
// Ranges (U7, third part): a disk-cold join's map and cache steps mostly read
// the base game's .vpk archives, which the game opened at start-up (so no open
// is seen during the join), and loose files under 1 MB. While a join loads,
// every ReadFile the game's file system makes (an import-table hook on
// filesystem_stdio, InstallReadHook) notes which 1 MB blocks of which file it
// reads; files not already on the list whole are kept as "range" lines, and
// only those blocks are warmed (measured 2026-11-07: about 455 MB of ~20
// archives of 100-265 MB each, instead of 3 GB for the whole archives).
//
// Warm: Start(address) reads that server's ranges, then its files, on a
// background thread at very low I/O priority, and drops the data (the OS keeps
// it in its cache). Only files still the recorded size, only on a drive
// without a seek penalty (an SSD), at most `budget` bytes, and it stops the
// moment a join starts (Cancel) or another server is warmed.
//
// Workshop items (U7, second part): the same hook also notes the Workshop item
// behind every .gma the join opens, whatever its size (Steam's content folder
// workshop\content\4000\<id>\, or the game's cache\workshop\<id>.gma), kept in
// the same list as "workshop\t<id>" lines. The menu asks Steam to bring those
// items up to date before the next join (home.lua).

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace prewarm
{
	// fs_cache's hook, after a successful read-only open while a join loads.
	// `size` is -1 when the game didn't ask for it.
	void Opened(const char* path, int64_t size);

	// The ReadFile hook on filesystem_stdio's imports (and CloseHandle, to drop
	// a closed handle's file). Idle unless a join is loading. Main thread.
	bool InstallReadHook(std::string& why);

	// A join is loading: start a fresh list (and stop any warming).
	void BeginJoin();
	// In game on `address` (checked by the caller): keep the list for it, with
	// what earlier joins of it learned (a rejoin in the same game start opens
	// only what isn't loaded already). Returns how many files were kept.
	int EndJoin(const std::wstring& dir, const std::string& address);

	// Whether a list is kept for `address`, and its total size.
	bool Recorded(const std::wstring& dir, const std::string& address, uint64_t* bytes = nullptr);
	// The Workshop items `address`'s last join mounted, in the order opened.
	std::vector<uint64_t> WorkshopItems(const std::wstring& dir, const std::string& address);

	// Warm `address`'s files (see above). False, with `why`, if it can't start.
	bool Start(const std::wstring& dir, const std::string& address, uint64_t budget, std::string& why);
	void Cancel();

	// One line for the status command.
	std::string Describe();

	namespace detail
	{
		constexpr int64_t kBlock = 1 << 20;   // ranges are kept in 1 MB blocks

		// The file name a server's list is kept under: the address with ':' as '_'.
		std::wstring FileFor(const std::wstring& dir, const std::string& address);
		// "path\tsize" lines to (path, size), skipping anything malformed;
		// "workshop\t<id>" lines to `workshop` and "range\tpath\tsize\ta-b,c-d"
		// lines (blocks, end not included) to `ranges` when they're given.
		// True if anything was read.
		struct Item { std::string path; int64_t size; };
		struct Range { std::string path; int64_t size; std::vector<std::pair<uint32_t, uint32_t>> runs; };
		bool Parse(const std::string& text, std::vector<Item>& out, std::vector<uint64_t>* workshop = nullptr,
			std::vector<Range>* ranges = nullptr);
		std::string Format(const std::vector<Item>& items, const std::vector<uint64_t>& workshop = {},
			const std::vector<Range>& ranges = {});
		// A join's list with what earlier joins of the server learned: this join's
		// files first, then earlier ones it didn't open (any case) that keep(item)
		// says are still there; the Workshop items of both. At most the usual caps.
		void Merge(std::vector<Item>& items, std::vector<uint64_t>& workshop, const std::vector<Item>& before,
			const std::vector<uint64_t>& workshopBefore, bool (*keep)(const Item&));
		// The same for ranges: a file on the whole-file list is left out; one in
		// both joins at the same size gets the blocks of both; an earlier one this
		// join didn't read stays if keep() says it's still there.
		void MergeRanges(std::vector<Range>& ranges, const std::vector<Range>& before, const std::vector<Item>& whole,
			bool (*keep)(const Item&));
		// A ReadFile while a join loads: `length` bytes at `offset` of `path` (`size`
		// bytes). The hook's work once it knows the file; the tests call it directly.
		void NoteRead(const std::string& path, int64_t size, uint64_t offset, uint64_t length);
		// The Workshop item behind a game file path, or 0: ...\workshop\content\4000\<id>\...
		// or ...\cache\workshop\<id>.gma (either slash, any case).
		uint64_t WorkshopId(const std::string& path);
		// Whether the drive holding `path` has no seek penalty (an SSD): 1 yes,
		// 0 no, -1 unknown.
		int NoSeekPenalty(const std::wstring& path);
	}
}
