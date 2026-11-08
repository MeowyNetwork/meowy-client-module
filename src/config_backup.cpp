#include "config_backup.h"
#include "file_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>

namespace config_backup
{
	const char* const kRestoreName = "meowy_restore.cfg";
	const char* const kRestoreDoneCommand = "meowy_restore_done";

	namespace
	{
		std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // ASCII names only

		// The whole file, or false if it's missing, empty, too big or unreadable.
		bool ReadAll(const std::wstring& path, std::string& out)
		{
			WIN32_FILE_ATTRIBUTE_DATA a;
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				return false;
			const unsigned long long size = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
			if (size == 0 || size > kMaxBytes)
				return false;
			out = file_util::ReadSmall(path, kMaxBytes);
			return out.size() == size;
		}
	}

	bool ValidName(const std::string& name)
	{
		// config-YYYYMMDD-HHMMSS.cfg
		if (name.size() != 26 || name.compare(0, 7, "config-") != 0 || name[15] != '-' || name.compare(22, 4, ".cfg") != 0)
			return false;
		for (size_t i = 7; i < 22; ++i)
			if (i != 15 && (name[i] < '0' || name[i] > '9'))
				return false;
		return true;
	}

	std::vector<Entry> List(const std::wstring& dir)
	{
		std::vector<Entry> list;
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((dir + L"\\config-*.cfg").c_str(), &f);
		if (h == INVALID_HANDLE_VALUE)
			return list;
		do
		{
			if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			std::string name;
			bool ascii = true;
			for (const wchar_t* c = f.cFileName; *c; ++c)
			{
				ascii = ascii && *c < 128;
				name += static_cast<char>(*c);
			}
			if (!ascii || !ValidName(name))
				continue;
			Entry e;
			e.name = name;
			e.when = name.substr(7, 4) + "-" + name.substr(11, 2) + "-" + name.substr(13, 2) + " " +
				name.substr(16, 2) + ":" + name.substr(18, 2) + ":" + name.substr(20, 2);
			e.size = (static_cast<unsigned long long>(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
			list.push_back(e);
		} while (FindNextFileW(h, &f));
		FindClose(h);
		std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.name > b.name; });
		return list;
	}

	bool Backup(const std::wstring& cfgFile, const std::wstring& dir, const std::string& stamp, std::string& result)
	{
		const std::string name = "config-" + stamp + ".cfg";
		if (!ValidName(name))
		{
			result = "bad time stamp";
			return false;
		}
		std::string text;
		if (!ReadAll(cfgFile, text))
		{
			result = "config.cfg couldn't be read";
			return false;
		}
		CreateDirectoryW(dir.c_str(), nullptr);
		const std::vector<Entry> before = List(dir);
		std::string newest;
		if (!before.empty() && ReadAll(dir + L"\\" + Widen(before.front().name), newest) && newest == text)
		{
			result = "unchanged";
			return true;
		}
		if (!before.empty() && before.front().name >= name)
		{
			result = "a copy at this time or later is already there";
			return false;
		}
		if (!file_util::WriteAtomic(dir + L"\\" + Widen(name), text))
		{
			result = "couldn't write the copy";
			return false;
		}
		const std::vector<Entry> after = List(dir);
		for (size_t i = kKeep; i < after.size(); ++i)
			DeleteFileW((dir + L"\\" + Widen(after[i].name)).c_str());
		result = "copied";
		return true;
	}

	bool Restore(const std::wstring& dir, const std::string& name, const std::wstring& cfgDir, std::string& why)
	{
		if (!ValidName(name))
		{
			why = "not a copy's name";
			return false;
		}
		std::string text;
		if (!ReadAll(dir + L"\\" + Widen(name), text))
		{
			why = "that copy couldn't be read";
			return false;
		}
		// Its last line says the game has run the rest (RestoreDoneCommand).
		if (!text.empty() && text.back() != '\n')
			text += '\n';
		text += std::string(kRestoreDoneCommand) + "\n";
		if (!file_util::WriteAtomic(cfgDir + L"\\" + Widen(kRestoreName), text))
		{
			why = "couldn't write to the game's cfg folder";
			return false;
		}
		why = kRestoreName;
		return true;
	}

	bool RemoveRestore(const std::wstring& cfgDir)
	{
		const std::wstring path = cfgDir + L"\\" + Widen(kRestoreName);
		if (DeleteFileW(path.c_str()))
			return true;
		const DWORD err = GetLastError();
		return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
	}

	std::wstring GameDirOf(const std::wstring& modulePath)
	{
		auto lower = [](std::wstring s) {
			for (wchar_t& c : s)
				if (c >= L'A' && c <= L'Z')
					c = static_cast<wchar_t>(c - L'A' + L'a');
			return s;
		};
		const size_t file = modulePath.find_last_of(L"\\/");
		if (file == std::wstring::npos || file < 8)
			return L"";
		// "\lua\bin" right before the file name, either slash.
		std::wstring tail = lower(modulePath.substr(file - 8, 8));
		for (wchar_t& c : tail)
			if (c == L'/')
				c = L'\\';
		if (tail != L"\\lua\\bin")
			return L"";
		return modulePath.substr(0, file - 8);
	}
}
