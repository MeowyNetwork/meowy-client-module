#pragma once
// Windows' installed-apps entry (Settings → Apps), which the installer registers
// under HKCU (installer/Installer.cs AppsKey). The pack keeps its version right:
// an in-game update changes the running version without the installer, so at
// every start the pack writes its own version there, when the entry exists.
#include <string>

namespace apps_entry
{
	// The installer's key, under HKEY_CURRENT_USER.
	constexpr wchar_t kKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MeowyClientPack";

	// Writes `version` ("1.7.3") as DisplayVersion (plus VersionMajor/VersionMinor)
	// under HKCU\<key>, only if that key exists and the value differs. Never
	// creates the key: no entry means the pack wasn't installed by the installer
	// (or was uninstalled), and that stays so. True when it wrote.
	bool RefreshVersion(const std::wstring& key, const std::string& version);
}
