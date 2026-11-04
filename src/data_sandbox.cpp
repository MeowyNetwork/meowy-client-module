#include "data_sandbox.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

namespace
{
	const wchar_t kMenu[] = L"menu";

	bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
	bool IsDir(const std::wstring& p)
	{
		DWORD a = GetFileAttributesW(p.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
	}

	std::wstring Widen(const std::string& s)
	{
		std::wstring w;
		for (char c : s)
			w += static_cast<wchar_t>(static_cast<unsigned char>(c));
		return w;
	}

	std::string Narrow(const std::wstring& w)
	{
		std::string s;
		for (wchar_t c : w)
			s += c < 128 ? static_cast<char>(c) : '?';
		return s;
	}

	// The folder that holds this module (with a trailing backslash).
	std::wstring OwnFolder()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&OwnFolder), &self);
		wchar_t path[MAX_PATH];
		DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
		std::wstring s(path, n < MAX_PATH ? n : 0);
		size_t slash = s.find_last_of(L"\\/");
		return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
	}

	// garrysmod\data: the game's working folder is the install root (GarrysMod\);
	// failing that, this module is in garrysmod\lua\bin.
	std::wstring DataDir()
	{
		static std::wstring dir;
		if (!dir.empty())
			return dir;
		wchar_t cwd[MAX_PATH];
		DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
		if (n && n < MAX_PATH && IsDir(std::wstring(cwd) + L"\\garrysmod\\data"))
			return dir = std::wstring(cwd) + L"\\garrysmod\\data";
		std::wstring own = OwnFolder();
		if (!own.empty() && IsDir(own + L"..\\..\\data"))
			return dir = own + L"..\\..\\data";
		return dir;
	}

	std::wstring Volume(const std::wstring& path)
	{
		wchar_t v[MAX_PATH];
		return GetVolumePathNameW(path.c_str(), v, MAX_PATH) ? std::wstring(v) : std::wstring();
	}

	// The vault: %LOCALAPPDATA%\MeowyClientPack\vault when that's on the game's
	// drive (moves stay renames), else steamapps\MeowyClientPackVault, beside the
	// game but outside its folder. Both are out of reach of GMod's file paths.
	std::wstring VaultDir()
	{
		static std::wstring dir;
		if (!dir.empty())
			return dir;
		std::wstring data = DataDir();
		if (data.empty())
			return dir;
		wchar_t base[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		if (n && n < MAX_PATH && Volume(base) == Volume(data))
		{
			std::wstring pack = std::wstring(base) + L"\\MeowyClientPack";
			CreateDirectoryW(pack.c_str(), nullptr);
			dir = pack + L"\\vault";
		}
		else
		{
			// data -> garrysmod -> GarrysMod -> common -> steamapps
			dir = data + L"\\..\\..\\..\\..\\MeowyClientPackVault";
		}
		CreateDirectoryW(dir.c_str(), nullptr);
		return dir;
	}

	// "1.2.3.4:27015" -> "1.2.3.4_27015"; anything odd becomes '_'.
	std::wstring KeyFor(const std::string& address)
	{
		std::wstring k;
		for (char c : address.substr(0, 64))
			k += (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ? static_cast<wchar_t>(c) : L'_';
		return k.empty() ? L"unknown" : k;
	}

	std::wstring ReadActive()
	{
		std::wstring path = VaultDir() + L"\\active";
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return L"";
		char buf[128];
		DWORD got = 0;
		ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
		CloseHandle(h);
		return Widen(std::string(buf, got));
	}

	void WriteActive(const std::wstring& key)
	{
		std::wstring path = VaultDir() + L"\\active";
		HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		std::string s = Narrow(key);
		DWORD wrote = 0;
		WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &wrote, nullptr);
		FlushFileBuffers(h);
		CloseHandle(h);
	}

	std::vector<std::wstring> Entries(const std::wstring& dir)
	{
		std::vector<std::wstring> out;
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
		if (h == INVALID_HANDLE_VALUE)
			return out;
		do
		{
			if (std::wcscmp(f.cFileName, L".") && std::wcscmp(f.cFileName, L".."))
				out.push_back(f.cFileName);
		} while (FindNextFileW(h, &f));
		FindClose(h);
		return out;
	}

	// Move `src` to `dst`: a rename, or for a folder whose target already
	// exists, its contents one by one. A file in use stays where it is.
	void MoveTree(const std::wstring& src, const std::wstring& dst)
	{
		if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
			return;
		if (IsDir(src) && (IsDir(dst) || CreateDirectoryW(dst.c_str(), nullptr)))
		{
			for (const std::wstring& e : Entries(src))
				MoveTree(src + L"\\" + e, dst + L"\\" + e);
			RemoveDirectoryW(src.c_str());
		}
	}

	// Everything in `from` into `to` (created if missing).
	void MoveAll(const std::wstring& from, const std::wstring& to)
	{
		CreateDirectoryW(to.c_str(), nullptr);
		for (const std::wstring& e : Entries(from))
			MoveTree(from + L"\\" + e, to + L"\\" + e);
	}

	// data/ now holds `current`'s files; make it hold `next`'s.
	void Switch(const std::wstring& current, const std::wstring& next)
	{
		const std::wstring data = DataDir(), vault = VaultDir();
		if (data.empty() || vault.empty())
			return;
		// Before the first switch ever, data/ is everything from before the
		// sandbox: kept apart in "before-sandbox", never shown to a server.
		const std::wstring from = current.empty() ? L"before-sandbox" : current;
		WriteActive(L"switching:" + next);
		MoveAll(data, vault + L"\\" + from);
		MoveAll(vault + L"\\" + next, data);
		RemoveDirectoryW((vault + L"\\" + next).c_str());
		WriteActive(next);
	}

	std::wstring g_active; // cached; the file is the truth across restarts
	bool g_loaded = false;

	std::wstring Active()
	{
		if (!g_loaded)
		{
			g_active = ReadActive();
			g_loaded = true;
		}
		return g_active;
	}
}

namespace data_sandbox
{
	void Recover()
	{
		std::wstring a = Active();
		if (a.rfind(L"switching:", 0) == 0)
		{
			// Went down mid-switch: data/ may be a mix. Put what's there with the
			// target it was switching to, then go back to the menu's.
			std::wstring target = a.substr(10);
			const std::wstring data = DataDir(), vault = VaultDir();
			if (!data.empty() && !vault.empty())
				MoveAll(data, vault + L"\\" + target);
			a = target;
		}
		if (!a.empty() && a != kMenu)
		{
			Switch(a, kMenu);
			g_active = kMenu;
		}
	}

	void Enter(const std::string& address)
	{
		if (address.empty())
			return;
		std::wstring key = KeyFor(address);
		std::wstring a = Active();
		if (a == key)
			return;
		Switch(a, key);
		g_active = key;
	}

	void Leave()
	{
		std::wstring a = Active();
		if (a == kMenu)
			return;
		Switch(a, kMenu);
		g_active = kMenu;
	}

	std::string Describe()
	{
		return "data/ holds " + (Active().empty() ? std::string("everything (not sandboxed yet)") : Narrow(Active())) + "; vault " + Narrow(VaultDir());
	}
}

#else

namespace data_sandbox
{
	void Recover() {}
	void Enter(const std::string&) {}
	void Leave() {}
	std::string Describe() { return "unsupported"; }
}

#endif
