#include "crash_reports.h"

#include <algorithm>

#ifndef _WIN32

namespace crashes
{
	std::vector<Report> Since(long long, size_t) { return {}; }
	std::vector<Report> SinceIn(const std::wstring&, long long, size_t) { return {}; }
	std::wstring Root() { return std::wstring(); }
	std::vector<Candidate> Candidates(const std::wstring&, long long) { return {}; }
	bool Read(const std::wstring&, const Candidate&, Report&) { return false; }
}

#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdio>
#include <cwchar>
#include <fstream>

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

	bool IsAsciiAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
	bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }
	bool IsAsciiAlnum(char c) { return IsAsciiAlpha(c) || IsAsciiDigit(c); }
}

namespace crashes
{
	// Error strings inside the traces can quote absolute paths, which carry the
	// install folder and often the Windows user name. Each one ("C:\...",
	// "C:/...", "\\host\...") is replaced by "<path>" up to where the path ends:
	// a line end, a quote, or the ':' before a line number or the error text
	// ("...\x.lua:12: attempt to..."). Spaces don't end it, so
	// "C:\Users\Jane Doe\..." is masked whole. Linear, no std::regex (which can
	// throw on long input in MSVC's implementation).
	std::string MaskPaths(const std::string& s)
	{
		std::string out;
		out.reserve(s.size());
		size_t i = 0;
		while (i < s.size())
		{
			const bool boundary = i == 0 || !IsAsciiAlnum(s[i - 1]);
			const bool drive = boundary && i + 2 < s.size() && IsAsciiAlpha(s[i]) && s[i + 1] == ':' && (s[i + 2] == '\\' || s[i + 2] == '/');
			const bool unc = boundary && i + 2 < s.size() && s[i] == '\\' && s[i + 1] == '\\' && IsAsciiAlnum(s[i + 2]);
			if (!drive && !unc)
			{
				out += s[i++];
				continue;
			}
			size_t j = i + (drive ? 3 : 2);
			while (j < s.size())
			{
				const char c = s[j];
				if (c == '\n' || c == '"' || c == '\'')
					break;
				if (c == ':' && j + 1 < s.size() && (IsAsciiDigit(s[j + 1]) || s[j + 1] == ' '))
					break;
				++j;
			}
			out += "<path>";
			i = j;
		}
		return out;
	}
}

namespace
{
	using crashes::MaskPaths;

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
		section = MaskPaths(section);
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

	std::wstring Root()
	{
		return GameRoot();
	}

	std::vector<Candidate> Candidates(const std::wstring& root, long long since)
	{
		std::vector<Candidate> out;
		if (root.empty())
			return out;
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((root + L"\\crashes\\*.txt").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE)
			return out;
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			const long long t = FileTimeToUnix(fd.ftLastWriteTime);
			if (t <= since)
				continue;
			Candidate c;
			c.file = fd.cFileName;
			c.name = Narrow(fd.cFileName, std::wcslen(fd.cFileName));
			c.time = t;
			out.push_back(std::move(c));
		} while (FindNextFileW(h, &fd));
		FindClose(h);
		return out;
	}

	bool Read(const std::wstring& root, const Candidate& c, Report& out)
	{
		if (root.empty() || c.file.size() < 4)
			return false;
		const std::wstring txt = root + L"\\crashes\\" + c.file;
		std::string trace = LuaSection(txt);
		if (trace.empty())
			return false;
		out = Report();
		out.name = c.name;
		out.time = c.time;
		out.luaTrace = std::move(trace);
		ReadFault(txt.substr(0, txt.size() - 4) + L".dmp", out);
		return true;
	}

	// Listed, sorted newest first, then read one at a time until `max` have
	// a Lua trace: the same reports as reading them all and keeping the
	// newest `max`, without opening the older ones.
	std::vector<Report> SinceIn(const std::wstring& root, long long since, size_t max)
	{
		std::vector<Report> out;
		if (root.empty() || max == 0)
			return out;
		std::vector<Candidate> list = Candidates(root, since);
		std::stable_sort(list.begin(), list.end(), [](const Candidate& a, const Candidate& b) { return a.time > b.time; });
		for (const Candidate& c : list)
		{
			if (out.size() >= max)
				break;
			Report r;
			if (Read(root, c, r))
				out.push_back(std::move(r));
		}
		return out;
	}
}

#endif
