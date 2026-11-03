#include "crash_reports.h"

#include <algorithm>

#ifndef _WIN32

namespace crashes
{
	std::vector<Report> Since(long long, size_t) { return {}; }
	std::vector<Report> SinceIn(const std::wstring&, long long, size_t) { return {}; }
}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdio>
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

	std::string Narrow(const wchar_t* s, size_t len)
	{
		int n = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
		if (n > 0)
			WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), &out[0], n, nullptr, nullptr);
		return out;
	}

	// "engine.dll+0x34b67c (0xC0000409)" from the minidump the game wrote
	// beside the report: the exception record says where it crashed, the
	// module list says in which module. Only the file name is kept, never the
	// path. Read-only, through a mapped view of the file.
	std::string Fault(const std::wstring& dmpPath)
	{
		HANDLE file = CreateFileW(dmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return std::string();
		LARGE_INTEGER fileSize{};
		GetFileSizeEx(file, &fileSize);
		const ULONG64 bytes = static_cast<ULONG64>(fileSize.QuadPart);
		HANDLE mapping = bytes > 0 ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
		void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
		std::string out;

		PMINIDUMP_DIRECTORY dir = nullptr;
		void* stream = nullptr;
		ULONG size = 0;
		if (view && MiniDumpReadDumpStream(view, ExceptionStream, &dir, &stream, &size) && stream)
		{
			const auto* ex = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);
			ULONG64 addr = ex->ExceptionRecord.ExceptionAddress;
			char code[16];
			snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(ex->ExceptionRecord.ExceptionCode));

			std::string where = "no module";   // e.g. a call through a bad pointer
			void* modStream = nullptr;
			if (MiniDumpReadDumpStream(view, ModuleListStream, &dir, &modStream, &size) && modStream)
			{
				const auto* list = static_cast<const MINIDUMP_MODULE_LIST*>(modStream);
				for (ULONG i = 0; i < list->NumberOfModules; ++i)
				{
					const MINIDUMP_MODULE& m = list->Modules[i];
					if (addr < m.BaseOfImage || addr >= m.BaseOfImage + m.SizeOfImage)
						continue;
					// The name lives at an offset inside the file; never read past its end.
					if (static_cast<ULONG64>(m.ModuleNameRva) + sizeof(ULONG32) > bytes)
						break;
					const auto* name = reinterpret_cast<const MINIDUMP_STRING*>(static_cast<const char*>(view) + m.ModuleNameRva);
					if (static_cast<ULONG64>(m.ModuleNameRva) + sizeof(ULONG32) + name->Length > bytes)
						break;
					std::wstring full(name->Buffer, name->Length / sizeof(wchar_t));
					size_t slash = full.find_last_of(L"\\/");
					std::string base = Narrow(full.c_str() + (slash == std::wstring::npos ? 0 : slash + 1),
						full.size() - (slash == std::wstring::npos ? 0 : slash + 1));
					char off[32];
					snprintf(off, sizeof(off), "+0x%llx", static_cast<unsigned long long>(addr - m.BaseOfImage));
					where = base + off;
					break;
				}
			}
			out = where + " (" + code + ")";
		}

		if (view) UnmapViewOfFile(view);
		if (mapping) CloseHandle(mapping);
		CloseHandle(file);
		return out;
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
		return SinceIn(GameRoot(), since, max);
	}

	std::vector<Report> SinceIn(const std::wstring& root, long long since, size_t max)
	{
		std::vector<Report> out;
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
			std::wstring txt = root + L"\\crashes\\" + fd.cFileName;
			std::string trace = LuaSection(txt);
			if (!trace.empty())
				out.push_back({ t, trace, Fault(txt.substr(0, txt.size() - 4) + L".dmp") });
		} while (FindNextFileW(h, &fd));
		FindClose(h);

		std::sort(out.begin(), out.end(), [](const Report& a, const Report& b) { return a.time > b.time; });
		if (out.size() > max)
			out.resize(max);
		return out;
	}
}

#endif
