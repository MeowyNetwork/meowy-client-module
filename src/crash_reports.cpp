#include "crash_reports.h"

#include <algorithm>

#ifndef _WIN32

namespace crashes
{
	std::vector<Report> Since(long long, size_t) { return {}; }
}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fstream>
#include <regex>

namespace
{
	constexpr size_t MAX_TRACE_BYTES = 4096;
	constexpr size_t MAX_FILE_BYTES = 256 * 1024;

	long long FileTimeToUnix(const FILETIME& ft)
	{
		ULARGE_INTEGER t;
		t.LowPart = ft.dwLowDateTime;
		t.HighPart = ft.dwHighDateTime;
		return static_cast<long long>(t.QuadPart / 10000000ULL) - 11644473600LL;
	}

	// The game's root folder (the one holding garrysmod/ and crashes/), found by
	// walking up from gmod.exe / hl2.exe until a garrysmod subfolder appears.
	std::wstring GameRoot()
	{
		wchar_t buf[MAX_PATH * 2];
		DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
		if (n == 0)
			return std::wstring();
		std::wstring dir(buf, n);
		for (int i = 0; i < 4; ++i)
		{
			size_t slash = dir.find_last_of(L"\\/");
			if (slash == std::wstring::npos)
				break;
			dir.resize(slash);
			DWORD attr = GetFileAttributesW((dir + L"\\garrysmod").c_str());
			if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
				return dir;
		}
		return std::wstring();
	}

	std::string LuaSection(const std::wstring& path)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f)
			return std::string();
		std::string text(MAX_FILE_BYTES, '\0');
		f.read(&text[0], static_cast<std::streamsize>(text.size()));
		text.resize(static_cast<size_t>(f.gcount()));

		size_t start = text.find("-Lua Stack Traces-");
		if (start == std::string::npos)
			return std::string();
		std::string section = text.substr(start);
		section.erase(std::remove(section.begin(), section.end(), '\r'), section.end());
		// Error strings inside the traces can quote absolute paths; mask them.
		static const std::regex drivePath(R"([A-Za-z]:[\\/][^\s"':]*)");
		section = std::regex_replace(section, drivePath, "<path>");
		if (section.size() > MAX_TRACE_BYTES)
			section.resize(MAX_TRACE_BYTES);
		return section;
	}
}

namespace crashes
{
	std::vector<Report> Since(long long since, size_t max)
	{
		std::vector<Report> out;
		std::wstring root = GameRoot();
		if (root.empty() || max == 0)
			return out;

		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((root + L"\\crashes\\*.txt").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE)
			return out;
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			long long t = FileTimeToUnix(fd.ftLastWriteTime);
			if (t <= since)
				continue;
			std::string trace = LuaSection(root + L"\\crashes\\" + fd.cFileName);
			if (!trace.empty())
				out.push_back({ t, trace });
		} while (FindNextFileW(h, &fd));
		FindClose(h);

		std::sort(out.begin(), out.end(), [](const Report& a, const Report& b) { return a.time > b.time; });
		if (out.size() > max)
			out.resize(max);
		return out;
	}
}

#endif
