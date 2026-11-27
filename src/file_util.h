#pragma once
// Small settings files that must survive the game dying mid-write (Windows).

#include <string>

namespace file_util
{
	// How far a WriteAtomic goes to keep the file.
	enum class Durability
	{
		// Flushed to the disk before and after the rename: survives a power cut
		// or a Windows crash too. Several ms on an SSD, tens on a hard disk, so
		// only for what must never go back (the updater's and the policy's files).
		Durable,
		// Written and renamed, not flushed: survives the game (or any process)
		// crashing, since the data is already in Windows' cache, but after a
		// power cut the file can be the old one or empty. For session markers,
		// held cvars, menu settings and other lists that only need to outlive
		// a game crash.
		Relaxed,
	};

	// Replaces `path` with `text` as a whole: written to "<path>.tmp", then
	// renamed over `path` (Durable: flushed to disk first; see Durability). A
	// crash leaves the old file or the new one, never a half-written one. False
	// on any failure (then `path` is unchanged).
	bool WriteAtomic(const std::wstring& path, const std::string& text, Durability d = Durability::Durable);

	// The first `max` bytes of `path`, or "" if it can't be read.
	std::string ReadSmall(const std::wstring& path, size_t max);
}
