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
#include <cwchar>
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

	// What ParseFault finds in a dump: plain data, so the parser needs no C++
	// objects and can run under structured exception handling.
	struct RawFault
	{
		bool found = false;
		ULONG32 code = 0;
		ULONG64 offset = 0;
		wchar_t module[MAX_PATH] = {};   // the module's file name only, or empty
	};

	// Is the stream [p, p + size) inside the mapped file?
	bool InView(const void* view, ULONG64 bytes, const void* p, ULONG64 size)
	{
		const char* base = static_cast<const char*>(view);
		const char* at = static_cast<const char*>(p);
		if (at < base)
			return false;
		const ULONG64 offset = static_cast<ULONG64>(at - base);
		return offset <= bytes && size <= bytes - offset;
	}

	// The exception record says at which address, the module list in which
	// module. Every stream and offset is checked against the file's size, and
	// a dump cut short mid-write (or otherwise broken) is "no fault", never a
	// crash: a read outside the view is caught.
	bool ParseFault(const void* view, ULONG64 bytes, RawFault& out)
	{
		__try
		{
			PMINIDUMP_DIRECTORY dir = nullptr;
			void* stream = nullptr;
			ULONG size = 0;
			if (!MiniDumpReadDumpStream(const_cast<void*>(view), ExceptionStream, &dir, &stream, &size) || !stream
				|| size < sizeof(MINIDUMP_EXCEPTION_STREAM) || !InView(view, bytes, stream, size))
				return false;
			const auto* ex = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(stream);
			const ULONG64 addr = ex->ExceptionRecord.ExceptionAddress;
			out.code = ex->ExceptionRecord.ExceptionCode;
			out.offset = addr;     // stays the raw address when no module holds it
			out.found = true;

			void* modStream = nullptr;
			ULONG modSize = 0;
			if (!MiniDumpReadDumpStream(const_cast<void*>(view), ModuleListStream, &dir, &modStream, &modSize) || !modStream
				|| modSize < FIELD_OFFSET(MINIDUMP_MODULE_LIST, Modules) || !InView(view, bytes, modStream, modSize))
				return true;
			const auto* list = static_cast<const MINIDUMP_MODULE_LIST*>(modStream);
			// The count comes from the file: only as many modules as the stream holds.
			const ULONG64 room = (modSize - FIELD_OFFSET(MINIDUMP_MODULE_LIST, Modules)) / sizeof(MINIDUMP_MODULE);
			const ULONG64 count = list->NumberOfModules < room ? list->NumberOfModules : room;
			for (ULONG64 i = 0; i < count; ++i)
			{
				const MINIDUMP_MODULE& m = list->Modules[i];
				if (addr < m.BaseOfImage || addr - m.BaseOfImage >= m.SizeOfImage)
					continue;
				// The name lives at an offset inside the file; never read past its end.
				if (static_cast<ULONG64>(m.ModuleNameRva) + sizeof(ULONG32) > bytes)
					break;
				const auto* name = reinterpret_cast<const MINIDUMP_STRING*>(static_cast<const char*>(view) + m.ModuleNameRva);
				if (static_cast<ULONG64>(m.ModuleNameRva) + sizeof(ULONG32) + name->Length > bytes)
					break;
				const size_t len = name->Length / sizeof(wchar_t);
				size_t from = 0;
				for (size_t c = 0; c < len; ++c)
					if (name->Buffer[c] == L'\\' || name->Buffer[c] == L'/')
						from = c + 1;
				size_t n = 0;
				for (size_t c = from; c < len && n + 1 < MAX_PATH; ++c)
					out.module[n++] = name->Buffer[c];
				out.module[n] = L'\0';
				out.offset = addr - m.BaseOfImage;
				break;
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// Where the game crashed, from the minidump it wrote beside the report.
	// Only the module's file name is kept, never its path. Read-only, through a
	// mapped view of the file.
	void ReadFault(const std::wstring& dmpPath, crashes::Report& r)
	{
		HANDLE file = CreateFileW(dmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return;
		LARGE_INTEGER fileSize{};
		GetFileSizeEx(file, &fileSize);
		const ULONG64 bytes = static_cast<ULONG64>(fileSize.QuadPart);
		HANDLE mapping = bytes > 0 ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
		void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;

		RawFault raw;
		if (view && ParseFault(view, bytes, raw) && raw.found)
		{
			r.faultCode = raw.code;
			r.faultOffset = raw.offset;
			r.faultModule = Narrow(raw.module, std::wcslen(raw.module));
			char text[96];
			snprintf(text, sizeof(text), "%s+0x%llx (0x%08lX)",
				r.faultModule.empty() ? "no module" : r.faultModule.c_str(),
				static_cast<unsigned long long>(r.faultOffset), static_cast<unsigned long>(r.faultCode));
			r.fault = text;
		}

		if (view) UnmapViewOfFile(view);
		if (mapping) CloseHandle(mapping);
		CloseHandle(file);
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
			if (trace.empty())
				continue;
			Report r;
			r.time = t;
			r.luaTrace = trace;
			ReadFault(txt.substr(0, txt.size() - 4) + L".dmp", r);
			out.push_back(r);
		} while (FindNextFileW(h, &fd));
		FindClose(h);

		std::sort(out.begin(), out.end(), [](const Report& a, const Report& b) { return a.time > b.time; });
		if (out.size() > max)
			out.resize(max);
		return out;
	}
}

#endif
