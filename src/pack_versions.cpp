#include "pack_versions.h"
#include "file_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdlib>

namespace pack_versions
{
	namespace
	{
		std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // versions are ASCII

		bool Digits(const std::string& s, size_t maxLen)
		{
			if (s.empty() || s.size() > maxLen)
				return false;
			for (char c : s)
				if (c < '0' || c > '9')
					return false;
			return true;
		}

		std::vector<std::string> Lines(const std::string& text)
		{
			std::vector<std::string> out;
			size_t at = 0;
			while (at < text.size())
			{
				size_t end = text.find('\n', at);
				if (end == std::string::npos)
					end = text.size();
				std::string line = text.substr(at, end - at);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				out.push_back(line);
				at = end + 1;
			}
			return out;
		}
	}

	bool ValidVersion(const std::string& v)
	{
		size_t parts = 0, at = 0;
		while (at <= v.size())
		{
			size_t dot = v.find('.', at);
			if (dot == std::string::npos)
				dot = v.size();
			if (!Digits(v.substr(at, dot - at), 5) || ++parts > 4)
				return false;
			at = dot + 1;
		}
		return parts >= 1;
	}

	bool ValidSha256(const std::string& h)
	{
		if (h.size() != 64)
			return false;
		for (char c : h)
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
				return false;
		return true;
	}

	int Compare(const std::string& a, const std::string& b)
	{
		size_t i = 0, j = 0;
		while (i < a.size() || j < b.size())
		{
			unsigned long x = 0, y = 0;
			while (i < a.size() && a[i] != '.')
				x = x * 10 + static_cast<unsigned long>(a[i++] - '0');
			while (j < b.size() && b[j] != '.')
				y = y * 10 + static_cast<unsigned long>(b[j++] - '0');
			if (x != y)
				return x < y ? -1 : 1;
			if (i < a.size())
				++i;
			if (j < b.size())
				++j;
		}
		return 0;
	}

	const wchar_t* Arch() { return sizeof(void*) == 8 ? L"win64" : L"win32"; }
	std::wstring DllName() { return std::wstring(L"gmcl_meowy_") + Arch() + L".dll"; }

	std::wstring StatePath(const std::wstring& dataDir, const wchar_t* which)
	{
		return dataDir + L"\\state\\" + which + L"_" + Arch() + L".txt";
	}

	std::wstring VersionDll(const std::wstring& dataDir, const std::string& version)
	{
		return dataDir + L"\\bin\\" + Widen(version) + L"\\" + DllName();
	}

	bool ReadPointer(const std::wstring& path, Pointer* out)
	{
		Pointer p;
		bool haveVersion = false, haveHash = false;
		for (const std::string& line : Lines(file_util::ReadSmall(path, 512)))
		{
			if (line.compare(0, 8, "version ") == 0 && ValidVersion(line.substr(8)))
				p.version = line.substr(8), haveVersion = true;
			else if (line.compare(0, 7, "sha256 ") == 0 && ValidSha256(line.substr(7)))
				p.sha256 = line.substr(7), haveHash = true;
			else if (line.compare(0, 3, "ok ") == 0 && Digits(line.substr(3), 3))
				p.ok = std::stoi(line.substr(3));
		}
		if (!haveVersion || !haveHash)
			return false;
		if (out)
			*out = p;
		return true;
	}

	bool WritePointer(const std::wstring& path, const Pointer& p)
	{
		CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
		return file_util::WriteAtomic(path, "version " + p.version + "\nsha256 " + p.sha256 + "\nok " + std::to_string(p.ok) + "\n");
	}

	std::string Sha256File(const std::wstring& path)
	{
		HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (f == INVALID_HANDLE_VALUE)
			return std::string();
		BCRYPT_ALG_HANDLE alg = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::string out;
		LARGE_INTEGER size = {};
		if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= static_cast<LONGLONG>(kMaxDll)
			&& BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
			&& BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
		{
			std::vector<unsigned char> buf(1 << 16);
			DWORD read = 0;
			bool ok = true;
			while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) && read > 0)
				ok = BCryptHashData(hash, buf.data(), read, 0) == 0;
			unsigned char digest[32];
			if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0)
			{
				static const char* hex = "0123456789abcdef";
				for (unsigned char b : digest)
				{
					out += hex[b >> 4];
					out += hex[b & 15];
				}
			}
		}
		if (hash)
			BCryptDestroyHash(hash);
		if (alg)
			BCryptCloseAlgorithmProvider(alg, 0);
		CloseHandle(f);
		return out;
	}

	bool IsBad(const std::wstring& dataDir, const std::string& version)
	{
		for (const std::string& line : Lines(file_util::ReadSmall(StatePath(dataDir, L"bad"), 4096)))
			if (line == version)
				return true;
		return false;
	}

	void MarkBad(const std::wstring& dataDir, const std::string& version)
	{
		if (IsBad(dataDir, version))
			return;
		std::string text = file_util::ReadSmall(StatePath(dataDir, L"bad"), 4096);
		if (!text.empty() && text.back() != '\n')
			text += "\n";
		CreateDirectoryW((dataDir + L"\\state").c_str(), nullptr);
		file_util::WriteAtomic(StatePath(dataDir, L"bad"), text + version + "\n");
	}

	StubPolicy ReadStubPolicy(const std::wstring& dataDir)
	{
		StubPolicy p;
		for (const std::string& line : Lines(file_util::ReadSmall(dataDir + L"\\state\\policy.bin", 4096)))
		{
			if (line == "enabled 0")
				p.enabled = false;
			else if (line.compare(0, 12, "min_version ") == 0 && ValidVersion(line.substr(12)))
				p.minVersion = line.substr(12);
			else if (line.compare(0, 8, "revoked ") == 0 && ValidVersion(line.substr(8)))
				p.revoked.push_back(line.substr(8));
		}
		return p;
	}

	bool Allowed(const StubPolicy& p, const std::string& version)
	{
		if (!p.enabled)
			return false;
		if (!p.minVersion.empty() && Compare(version, p.minVersion) < 0)
			return false;
		for (const std::string& r : p.revoked)
			if (Compare(version, r) == 0)
				return false;
		return true;
	}

	bool UpdatesOff(const std::wstring& dataDir)
	{
		// The menu Lua's convar reads it the engine's way: a number, 0 is off.
		const std::string key = "meowy_updates=";
		for (const std::string& line : Lines(file_util::ReadSmall(dataDir + L"\\menu_settings.txt", 4096)))
			if (line.compare(0, key.size(), key) == 0)
				return std::atoi(line.c_str() + key.size()) == 0;
		return false;
	}

	std::string Promote(const std::wstring& dataDir)
	{
		Pointer pending;
		const std::wstring pendingPath = StatePath(dataDir, L"pending");
		if (!ReadPointer(pendingPath, &pending))
			return std::string();
		DeleteFileW(pendingPath.c_str());
		if (IsBad(dataDir, pending.version))
			return "update " + pending.version + " not taken: it failed before";
		if (Sha256File(VersionDll(dataDir, pending.version)) != pending.sha256)
			return "update " + pending.version + " not taken: its file doesn't match";
		Pointer current;
		if (ReadPointer(StatePath(dataDir, L"current"), &current) && current.version != pending.version)
			WritePointer(StatePath(dataDir, L"previous"), current);
		pending.ok = 0;
		WritePointer(StatePath(dataDir, L"current"), pending);
		return "updated to " + pending.version;
	}

	bool RevertAfterCrash(const std::wstring& dataDir, const std::string& version, std::string& said)
	{
		Pointer current;
		if (!ValidVersion(version) || !ReadPointer(StatePath(dataDir, L"current"), &current) || current.version != version || current.ok >= 2)
			return false;
		MarkBad(dataDir, version);
		Pointer previous;
		if (ReadPointer(StatePath(dataDir, L"previous"), &previous) && !IsBad(dataDir, previous.version))
		{
			WritePointer(StatePath(dataDir, L"current"), previous);
			DeleteFileW(StatePath(dataDir, L"previous").c_str());
			said = "the game closed while starting " + version + "; went back to " + previous.version;
		}
		else
		{
			DeleteFileW(StatePath(dataDir, L"current").c_str());
			said = "the game closed while starting " + version + "; went back to the installed copy";
		}
		return true;
	}

	Choice Choose(const std::wstring& dataDir)
	{
		Choice c;
		const StubPolicy policy = ReadStubPolicy(dataDir);
		for (const wchar_t* which : { L"current", L"previous" })
		{
			Pointer p;
			if (!ReadPointer(StatePath(dataDir, which), &p))
				continue;
			const std::wstring dll = VersionDll(dataDir, p.version);
			const char* skip = IsBad(dataDir, p.version) ? "it failed before"
				: !Allowed(policy, p.version) ? "Meowy Network's policy doesn't allow it"
				: Sha256File(dll) != p.sha256 ? "its file is missing or doesn't match"
				: nullptr;
			if (!skip)
			{
				c.path = dll;
				c.version = p.version;
				return c;
			}
			if (c.why.empty())
				c.why = p.version + " passed over: " + skip;
		}
		return c;
	}

	void NoteLoaded(const std::wstring& dataDir, const std::string& version)
	{
		Pointer current;
		if (!ReadPointer(StatePath(dataDir, L"current"), &current) || current.version != version || current.ok >= 99)
			return;
		++current.ok;
		WritePointer(StatePath(dataDir, L"current"), current);
	}
}
