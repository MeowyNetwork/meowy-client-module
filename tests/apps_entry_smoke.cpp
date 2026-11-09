// apps_entry: the pack keeps the installed-apps entry's version right, under a
// scratch HKCU key (never the real Uninstall key).
#include "../src/apps_entry.h"

#include <cstdio>
#include <string>
#include <windows.h>

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* what)
{
	++g_checks;
	if (!ok) ++g_fail;
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

static const wchar_t kRoot[] = L"Software\\MeowyClientPackTest";
static const wchar_t kKey[] = L"Software\\MeowyClientPackTest\\Uninstall\\MeowyClientPack";

static std::wstring Read(const wchar_t* name)
{
	wchar_t buf[64] = {};
	DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
	if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS)
		return L"(none)";
	return buf;
}
static DWORD ReadDword(const wchar_t* name)
{
	DWORD v = 0xFFFFFFFF, size = sizeof(v);
	RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size);
	return v;
}

int main()
{
	RegDeleteTreeW(HKEY_CURRENT_USER, kRoot);
	Check(!apps_entry::RefreshVersion(kKey, "1.7.3"), "no entry: nothing written");
	HKEY probe = nullptr;
	Check(RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &probe) != ERROR_SUCCESS, "and the key isn't created");
	if (probe) RegCloseKey(probe);

	HKEY k = nullptr;
	RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
	const wchar_t old[] = L"1.7.2";
	RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(old), sizeof(old));
	RegCloseKey(k);

	Check(apps_entry::RefreshVersion(kKey, "1.7.3"), "an older version is replaced");
	Check(Read(L"DisplayVersion") == L"1.7.3", "DisplayVersion is the running version");
	Check(ReadDword(L"VersionMajor") == 1 && ReadDword(L"VersionMinor") == 7, "VersionMajor/VersionMinor follow it");
	Check(!apps_entry::RefreshVersion(kKey, "1.7.3"), "the same version again: no write");
	Check(apps_entry::RefreshVersion(kKey, "1.10.0"), "a two-digit minor");
	Check(ReadDword(L"VersionMinor") == 10, "VersionMinor 10");
	Check(!apps_entry::RefreshVersion(kKey, ""), "an empty version is refused");
	Check(Read(L"DisplayVersion") == L"1.10.0", "and changes nothing");

	RegDeleteTreeW(HKEY_CURRENT_USER, kRoot);
	std::printf("%d checks, %d failed\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
