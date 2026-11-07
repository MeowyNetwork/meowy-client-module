#pragma once
// Copies of config.cfg, the game's saved settings and binds (Windows). The game
// rewrites it at every quit with whatever is set by then, including what a
// server's Lua changed, so the pack keeps the last few versions: one copy per
// start when it changed since the newest copy, the newest kKeep kept, named
// config-YYYYMMDD-HHMMSS.cfg. A copy goes back through the game's own exec: it's
// written to the game's cfg folder as meowy_restore.cfg (unbindall, binds,
// settings), and the menu runs "exec meowy_restore.cfg".
#include <string>
#include <vector>

namespace config_backup
{
	constexpr size_t kKeep = 10;
	constexpr size_t kMaxBytes = 1024 * 1024;   // config.cfg is tens of KB
	extern const char* const kRestoreName;      // "meowy_restore.cfg"

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

	// Writes copy `name` from `dir` to `cfgDir`\meowy_restore.cfg. False + why.
	bool Restore(const std::wstring& dir, const std::string& name, const std::wstring& cfgDir, std::string& why);
}
