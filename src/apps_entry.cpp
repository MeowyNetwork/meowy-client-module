#include "apps_entry.h"

#include <cstdlib>
#include <windows.h>

#pragma comment(lib, "advapi32.lib")

namespace apps_entry
{
	bool RefreshVersion(const std::wstring& key, const std::string& version)
	{
		if (version.empty() || version.size() > 32)
			return false;
		HKEY k = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) != ERROR_SUCCESS)
			return false;   // no entry: nothing to keep right
		const std::wstring want(version.begin(), version.end());
		wchar_t have[64] = {};
		DWORD type = 0, size = sizeof(have) - sizeof(wchar_t);
		const bool same = RegQueryValueExW(k, L"DisplayVersion", nullptr, &type, reinterpret_cast<BYTE*>(have), &size) == ERROR_SUCCESS &&
			type == REG_SZ && want == have;
		bool wrote = false;
		if (!same)
		{
			wrote = RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(want.c_str()),
				static_cast<DWORD>((want.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
			const DWORD major = static_cast<DWORD>(std::strtoul(version.c_str(), nullptr, 10));
			const size_t dot = version.find('.');
			const DWORD minor = dot == std::string::npos ? 0 : static_cast<DWORD>(std::strtoul(version.c_str() + dot + 1, nullptr, 10));
			RegSetValueExW(k, L"VersionMajor", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&major), sizeof(major));
			RegSetValueExW(k, L"VersionMinor", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&minor), sizeof(minor));
		}
		RegCloseKey(k);
		return wrote;
	}
}
