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
}

#else

namespace file_util
{
	bool WriteAtomic(const std::wstring&, const std::string&) { return false; }
}

#endif
