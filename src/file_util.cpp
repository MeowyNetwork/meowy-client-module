#include "file_util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace file_util
{
	bool WriteAtomic(const std::wstring& path, const std::string& text)
	{
		const std::wstring temp = path + L".tmp";
		HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		DWORD wrote = 0;
		bool ok = text.empty() || (WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr) && wrote == text.size());
		ok = FlushFileBuffers(h) && ok;
		CloseHandle(h);
		if (ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			return true;
		DeleteFileW(temp.c_str());
		return false;
	}

	std::string ReadSmall(const std::wstring& path, size_t max)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return std::string();
		std::string buf(max, '\0');
		DWORD n = 0;
		BOOL ok = ReadFile(h, &buf[0], static_cast<DWORD>(max), &n, nullptr);
		CloseHandle(h);
		buf.resize(ok ? n : 0);
		return buf;
	}
}

#else

namespace file_util
{
	bool WriteAtomic(const std::wstring&, const std::string&) { return false; }
	std::string ReadSmall(const std::wstring&, size_t) { return std::string(); }
}

#endif
