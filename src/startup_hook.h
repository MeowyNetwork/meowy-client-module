#pragma once
// The pack's start-up line in garrysmod/lua/includes/menu.lua (Windows).
//
// The installer adds one marked line there so the pack loads when the game
// starts (src/boot/boot.cpp). menu.lua is Garry's Mod's own file: an update or
// "Verify integrity of game files" puts the stock file back and the line is
// gone. The pack's own Lua is inside the DLL, so that line is all an update
// can take away. This puts it back:
//
// - Heal: when the pack loads from our server's client Lua and didn't start
//   with the game this session, the line is added again, unless the player
//   turned "Start with the game" off (startup_off.txt in the pack's data
//   folder; the installer's checkbox and Meowy settings both write it).
// - Set: Meowy settings' "Start with the game" toggle.
//
// The text rules are pure functions (WithLine, WithoutLine, HasLine) so
// tests/startup_hook_smoke.cpp checks them without a game.

#include <string>

namespace startup_hook
{
	// The marker every version's line carries; matched to find or remove it.
	extern const char* const kMarker;
	// The line itself, as the installer writes it.
	extern const char* const kLine;

	bool HasLine(const std::string& text);
	// text with the line appended (CRLF, like the file), or unchanged if present.
	std::string WithLine(const std::string& text);
	// text with every marked line removed, and the line break in front of it.
	std::string WithoutLine(const std::string& text);

	// Called by the menu-state entry point: the pack started with the game.
	void NoteMenuLoaded();
	bool MenuLoaded();

	// garrysmod: the game's garrysmod folder; dataDir: %LOCALAPPDATA%\MeowyClientPack.
	// Present: the line is in includes/menu.lua or menu/menu.lua.
	bool Present(const std::wstring& garrysmod);
	// The player turned start-up off.
	bool Off(const std::wstring& dataDir);
	// Adds the line back if it's missing and start-up isn't off. Returns what
	// it did: "present", "off", "added", or "failed: <why>".
	std::string Heal(const std::wstring& garrysmod, const std::wstring& dataDir);
	// on: add the line and forget "off"; off: remove it and remember "off".
	bool Set(const std::wstring& garrysmod, const std::wstring& dataDir, bool on);
}
