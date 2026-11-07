#pragma once
// Small settings files that must survive the game dying mid-write (Windows).

#include <string>

namespace file_util
{
	// Replaces `path` with `text` as a whole: written to "<path>.tmp", flushed to
	// disk, then renamed over `path`. A crash or power cut leaves the old file
	// or the new one, never an empty or half-written one. False on any failure
	// (then `path` is unchanged).
	bool WriteAtomic(const std::wstring& path, const std::string& text);
}
