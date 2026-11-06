#include "startup_hook.h"

#include <atomic>

namespace startup_hook
{
	const char* const kMarker = "meowy-client-pack";
	const char* const kLine = "pcall(require, \"meowy_boot\") -- meowy-client-pack v2 "
		"(loads the Meowy Client Pack at start-up; turn it off in Meowy settings, in the main menu)";

	bool HasLine(const std::string& text)
	{
		return text.find(kMarker) != std::string::npos;
	}

	std::string WithLine(const std::string& text)
	{
		if (HasLine(text))
			return text;
		std::string out = text;
		if (!out.empty() && out.back() != '\n')
			out += "\r\n";
		return out + kLine + "\r\n";
	}

	std::string WithoutLine(const std::string& text)
	{
		// As the installer's RemoveStartupHook: split on LF (each line keeps its
		// CR), drop marked lines, join, and drop the CRLF that WithLine put in
		// front of the line.
		std::string out;
		size_t start = 0;
		bool first = true;
		for (;;)
		{
			size_t nl = text.find('\n', start);
			std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
			if (line.find(kMarker) == std::string::npos)
			{
				if (!first)
					out += '\n';
				out += line;
				first = false;
			}
			if (nl == std::string::npos)
				break;
			start = nl + 1;
		}
		if (out.size() >= 2 && out.compare(out.size() - 2, 2, "\r\n") == 0)
			out.erase(out.size() - 2);
		return out;
	}

	namespace
	{
		std::atomic<bool> g_menuLoaded{ false };
	}

	void NoteMenuLoaded() { g_menuLoaded = true; }
	bool MenuLoaded() { return g_menuLoaded; }
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace startup_hook
{
	namespace
	{
		const wchar_t* const kHosts[] = { L"\\lua\\includes\\menu.lua", L"\\lua\\menu\\menu.lua" };

		bool ReadAll(const std::wstring& file, std::string& out)
		{
			HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return false;
			LARGE_INTEGER size{};
			bool ok = GetFileSizeEx(h, &size) && size.QuadPart < 4 * 1024 * 1024;
			if (ok)
			{
				out.resize(static_cast<size_t>(size.QuadPart));
				DWORD read = 0;
				ok = out.empty() || (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
			}
			CloseHandle(h);
			return ok;
		}

		// Write next to the file, then swap it in, so a crash never leaves half a menu.lua.
		bool WriteReplacing(const std::wstring& file, const std::string& text)
		{
			DWORD attrs = GetFileAttributesW(file.c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
				SetFileAttributesW(file.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
			const std::wstring temp = file + L".meowy-tmp";
			HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return false;
			DWORD written = 0;
			bool ok = text.empty() || (WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size());
			CloseHandle(h);
			if (ok)
				ok = MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
			if (!ok)
				DeleteFileW(temp.c_str());
			return ok;
		}

		std::wstring OffFile(const std::wstring& dataDir) { return dataDir + L"\\startup_off.txt"; }
	}

	bool Present(const std::wstring& garrysmod)
	{
		for (const wchar_t* host : kHosts)
		{
			std::string text;
			if (ReadAll(garrysmod + host, text) && HasLine(text))
				return true;
		}
		return false;
	}

	bool Off(const std::wstring& dataDir)
	{
		return !dataDir.empty() && GetFileAttributesW(OffFile(dataDir).c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	std::string Heal(const std::wstring& garrysmod, const std::wstring& dataDir)
	{
		if (Present(garrysmod))
			return "present";
		if (Off(dataDir))
			return "off";
		const std::wstring file = garrysmod + kHosts[0];
		std::string text;
		if (!ReadAll(file, text))
			return "failed: lua\\includes\\menu.lua not readable";
		if (!WriteReplacing(file, WithLine(text)))
			return "failed: couldn't write lua\\includes\\menu.lua (" + std::to_string(GetLastError()) + ")";
		return "added";
	}

	bool Set(const std::wstring& garrysmod, const std::wstring& dataDir, bool on)
	{
		bool ok = true;
		if (on)
		{
			DeleteFileW(OffFile(dataDir).c_str());
			const std::wstring file = garrysmod + kHosts[0];
			std::string text;
			if (!Present(garrysmod))
				ok = ReadAll(file, text) && WriteReplacing(file, WithLine(text));
		}
		else
		{
			for (const wchar_t* host : kHosts)
			{
				std::string text;
				if (ReadAll(garrysmod + host, text) && HasLine(text))
					ok = WriteReplacing(garrysmod + host, WithoutLine(text)) && ok;
			}
			HANDLE h = CreateFileW(OffFile(dataDir).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h != INVALID_HANDLE_VALUE)
			{
				const char note[] = "Start with the game is off (Meowy settings). Delete this file to turn it back on.\r\n";
				DWORD w = 0;
				WriteFile(h, note, sizeof(note) - 1, &w, nullptr);
				CloseHandle(h);
			}
			else
				ok = false;
		}
		return ok;
	}
}

#else

namespace startup_hook
{
	bool Present(const std::wstring&) { return false; }
	bool Off(const std::wstring&) { return false; }
	std::string Heal(const std::wstring&, const std::wstring&) { return "unsupported"; }
	bool Set(const std::wstring&, const std::wstring&, bool) { return false; }
}

#endif
