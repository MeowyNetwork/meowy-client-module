#pragma once

// Fewer missing-file checks while a server loads, for any server (roadmap 3.2).
//
// Every model, material and texture a join loads is looked for in each of the
// game's search folders in turn, and each folder that doesn't have it costs a
// failed open (CreateFile) through filesystem_stdio's FS_fopen. Measured
// 2026-11-07 on the first join of a session: open calls were 10.6 s of the
// 35 s before Lua starts, almost all of them from FS_fopen.
//
// While a join loads, FS_fopen for reading first asks a listing of the file's
// folder, read once with FindFirstFileEx: a name the folder doesn't have
// returns "not found" at once, exactly what the failed open would have; any
// other case opens as before. Opening for writing drops that folder's listing.
// Listings are kept between joins: in a new join a folder's listing is used
// again only if the folder's last-write time (which moves whenever a name is
// added, removed or renamed) is unchanged and it isn't a link; otherwise it's
// listed again. Folders anything else writes to during a join (data, download,
// cache, Steam's workshop folder, cfg) are never listed.
//
// The swap is the filesystem object's vtable pointer to a copy with that one
// slot replaced: no code bytes change, and it refuses unless the object is
// filesystem_stdio's CFileSystem_Stdio and the slot's function is FS_fopen
// (its "rb" check, byte for byte).

#include <cstdint>
#include <string>

namespace fs_cache
{
	// Puts the hook in place (inactive). Main thread, at the menu.
	bool Install(std::string& why);
	bool Uninstall();

	// The join window: on while a join loads, off once in game. Each start of
	// a window is a new join (listings are checked again before reuse).
	void SetActive(bool on);

	std::string Describe();

	namespace detail
	{
		// A full path as FS_fopen gets it -> lowercase folder and name, or false
		// if it's anything but a plain "X:\..." path the cache can answer for.
		bool Split(const char* path, std::string& dir, std::string& name);

		// A folder something else writes to during a join: never cached.
		bool Excluded(const std::string& dir);

		enum class Answer { Present, Absent, Unknown };

		// What the cache says about one name in one folder, listing the folder
		// on first use.
		Answer Lookup(const std::string& dir, const std::string& name);
		void Invalidate(const std::string& dir);
		void Clear();
		void NewJoin();   // what SetActive(true) does to the listings
		size_t Folders();
	}
}
