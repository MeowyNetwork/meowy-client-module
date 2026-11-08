#pragma once
// Copies of config.cfg, the game's saved settings and binds (Windows). The game
// rewrites it at every quit with whatever is set by then, including what a
// server's Lua changed, so the pack keeps the last few versions: one copy per
// start when it changed since the newest copy, the newest kKeep kept, named
// config-YYYYMMDD-HHMMSS.cfg. A copy goes back through the game's own exec: it's
// written to the game's cfg folder as meowy_restore.cfg (unbindall, binds,
// settings), the menu runs "exec meowy_restore.cfg", and the file is removed
// again once the game has run it: its last line is kRestoreDoneCommand, which
// the menu answers by removing it (and at start, if one was left behind).
#include <string>
#include <vector>

namespace config_backup
{
	constexpr size_t kKeep = 10;
	constexpr size_t kMaxBytes = 1024 * 1024;   // config.cfg is tens of KB
	extern const char* const kRestoreName;      // "meowy_restore.cfg"
	extern const char* const kRestoreDoneCommand;   // "meowy_restore_done", the file's last line

	struct Entry
	{
		std::string name;    // config-YYYYMMDD-HHMMSS.cfg
		std::string when;    // YYYY-MM-DD HH:MM:SS
		unsigned long long size = 0;
	};

	// Only names this module makes: config-, 8 digits, -, 6 digits, .cfg.
	bool ValidName(const std::string& name);

	// Copies `cfgFile` into `dir` as config-<stamp>.cfg (stamp YYYYMMDD-HHMMSS)
	// unless it's the same as the newest copy; then keeps the newest kKeep.
	// `result` is "copied" or "unchanged", or why not (returns false).
	bool Backup(const std::wstring& cfgFile, const std::wstring& dir, const std::string& stamp, std::string& result);

	// The copies in `dir`, newest first.
	std::vector<Entry> List(const std::wstring& dir);

	// Writes copy `name` from `dir` to `cfgDir`\meowy_restore.cfg, with
	// kRestoreDoneCommand as its last line. False + why.
	bool Restore(const std::wstring& dir, const std::string& name, const std::wstring& cfgDir, std::string& why);

	// Removes `cfgDir`\meowy_restore.cfg: the game has run it, or it was left
	// behind. True when it's gone, or was never there.
	bool RemoveRestore(const std::wstring& cfgDir);

	// The game's garrysmod folder from the path of a module the installer put in
	// its lua\bin ("...\garrysmod\lua\bin\x.dll" -> "...\garrysmod"), or "" for a
	// module anywhere else: an update runs from
	// %LOCALAPPDATA%\MeowyClientPack\bin\<version> (pack_versions.h).
	std::wstring GameDirOf(const std::wstring& modulePath);
}
