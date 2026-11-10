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
// other case opens as before. Opening for writing drops that folder's listing
// and the listings of every folder above it, before the open and again after
// it. Folders anything else writes to during a join (data, download, cache,
// Steam's workshop folder, cfg) are never listed.
//
// The hook runs on whatever thread opens the file, mostly the game's main
// thread, so a folder it already knows costs no allocation and no system call:
// the path is split into a buffer on the stack, folders are found by a 64-bit
// hash of their path, and each listing keeps a sorted list of 64-bit hashes of
// its names (two names with the same hash can only turn "absent" into
// "present", which just opens the file as before).
//
// A folder it doesn't know yet:
//  - If the nearest folder above it that this join already knows is absent, or
//    was listed this join without the next name on the way down, it's absent
//    too: no system call. Never from a link, a folder it couldn't read, or one
//    not checked this join. A write drops every folder above the one it writes
//    in, so a folder a write just made is never answered "absent" this way.
//  - Otherwise it's listed. A folder that isn't there costs that one call. A
//    folder it can't read (access denied, say) is kept as "can't tell" for the
//    rest of the join: one failed listing per join, not one per open.
//
// Listings are kept between joins: in a new join a folder's listing is used
// again only if the folder's last-write time (which moves whenever a name is
// added, removed or renamed) is unchanged and it isn't a link; otherwise it's
// listed again (or answered from the folder above). A listing not used in the
// last three joins is dropped.
//
// Why the write time is read after the listing, and compared with the time the
// listing started: the clock is read first (tStart), then the folder is
// listed, then its write time is read. A name changed before tStart is in the
// listing. A name changed after tStart moves the write time to about tStart or
// later, so the write time read afterwards isn't 2 s older than tStart and the
// listing isn't reused in a later join; a change after that read moves the time
// past the one kept, so the next join lists again. The 2 s are for the clock:
// a write time can be a tick (~15 ms) behind, and a second change in the same
// tick doesn't move it. Comparing with when the listing ended instead would let
// a name added early in a slow listing (over 2 s) be missed and still reused.
//
// A write and a listing at the same time: every drop counts up an epoch, and a
// listing (or an answer from the folder above) is kept only if the epoch hasn't
// moved since before it started; otherwise it answers "can't tell" for that
// one open. So a file a write creates during a join can't be cached as absent.
//
// The swap is the filesystem object's vtable pointer to a copy with that one
// slot replaced: no code bytes change, and it refuses unless the object is
// filesystem_stdio's CFileSystem_Stdio and the slot's function is FS_fopen
// (its "rb" check, byte for byte).

#include <cstddef>
#include <cstdint>
#include <string>

namespace fs_cache
{
	// Puts the hook in place (inactive). Main thread, at the menu.
	bool Install(std::string& why);
	bool Uninstall();

	// The join window: on while a join loads, off once in game. Each start of
	// a window is a new join (listings are checked again before reuse, and
	// those not used in the last three joins are dropped).
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

		// What the cache says about one name in one folder (both as Split gives
		// them), listing the folder on first use. Unknown for an excluded folder.
		Answer Lookup(const std::string& dir, const std::string& name);
		// What a write does: drops the folder and every folder above it.
		void Invalidate(const std::string& dir);
		void Clear();
		void NewJoin();   // what SetActive(true) does to the listings
		size_t Folders();

		// For the smoke test: a function called at these points of a lookup that
		// isn't answered from what's cached (null: none).
		enum class TestPoint
		{
			Listed,         // a folder was just read, before its write time is
			BeforeInsert,   // a new listing or answer is about to be kept
		};
		void SetTestHook(void (*hook)(TestPoint));

		// Install after an Uninstall: the object's vtable pointer (*slot) goes
		// back to `ours` (the copy kept from the first Install) if it's still
		// `original`, or is already `ours`. False, and nothing changed, if
		// something else's is there.
		bool SwapIn(void** slot, void* original, void* ours);
	}
}
