#include "launcher_pipe.h"
#include "server_gate.h"
#include "server_rep.h"

#include <atomic>
#include <cwctype>
#include <mutex>

namespace launcher_pipe
{
	const char* const kLua = R"lua(
-- The Meowy Launcher's pipe (src/launcher_pipe.cpp): tell it what the game is
-- doing, and run a join it asked for (an address the native side checked).
local nextLook, last = 0, nil
hook.Add("Think", "MeowyClientPack.Launcher", function()
	if SysTime() < nextLook then return end
	nextLook = SysTime() + 1
	local join = meowy.LauncherTakeJoin()
	-- JoinServer as resilience.lua does: RunConsoleCommand("connect", address)
	-- quotes the address and the engine answers "Bad server address".
	if isstring(join) and join ~= "" then
		if JoinServer then JoinServer(join) else RunConsoleCommand("connect", join) end
	end
	local state = IsInLoading() and "joining" or (IsInGame() and "ingame" or "menu")
	if state ~= last then
		last = state
		meowy.LauncherState(state)
	end
end)
)lua";

	namespace detail
	{
		bool ParseJoin(const std::string& line, std::string& address)
		{
			static const std::string head = "{\"t\":\"join\",\"a\":\"", tail = "\"}";
			if (line.size() <= head.size() + tail.size() || line.size() > 64 || line.compare(0, head.size(), head) != 0
				|| line.compare(line.size() - tail.size(), tail.size(), tail) != 0)
				return false;
			const std::string a = line.substr(head.size(), line.size() - head.size() - tail.size());
			for (char c : a)
				if (!((c >= '0' && c <= '9') || c == '.' || c == ':'))
					return false;
			if (!server_gate::IsOurAddress(a) && !server_rep::IsPublicAddress(a))
				return false;
			address = a;
			return true;
		}

		std::string StateLine(const std::string& state, const std::string& address)
		{
			if (state != "menu" && state != "joining" && state != "ingame")
				return "";
			std::string line = "{\"t\":\"state\",\"s\":\"" + state + "\"";
			// Only an address that is ours or public, and only its digits.
			if (state != "menu" && (server_gate::IsOurAddress(address) || server_rep::IsPublicAddress(address)))
				line += ",\"a\":\"" + address + "\"";
			return line + "}\n";
		}

		std::string HelloLine(const std::string& version)
		{
			std::string v;
			for (char c : version)
				if ((c >= '0' && c <= '9') || c == '.')
					v += c;
			return "{\"t\":\"hello\",\"v\":\"" + v.substr(0, 16) + "\"}\n";
		}

		bool SamePath(const std::wstring& image, const std::wstring& expected)
		{
			if (image.empty() || image.size() != expected.size())
				return false;
			for (size_t i = 0; i < image.size(); ++i)
			{
				wchar_t a = image[i] == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(image[i]));
				wchar_t b = expected[i] == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(expected[i]));
				if (a != b)
					return false;
			}
			return true;
		}

	}

	namespace
	{
		std::mutex g_lock;
		std::string g_state = "menu";
		std::string g_address;
		std::string g_join;
		std::atomic<bool> g_started{ false };
		std::atomic<bool> g_connected{ false };
		std::atomic<unsigned> g_stateVersion{ 1 };
		std::string g_version;
	}

	void SetState(const std::string& state, const std::string& address)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (state != g_state || address != g_address)
		{
			g_state = state;
			g_address = address;
			++g_stateVersion;
		}
	}

	std::string TakeJoin()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		std::string j;
		j.swap(g_join);
		return j;
	}

	bool Connected()
	{
		return g_connected;
	}
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <thread>

#pragma comment(lib, "advapi32.lib")

namespace launcher_pipe
{
	namespace
	{
		std::wstring UserSid()
		{
			HANDLE token = nullptr;
			if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
				return L"";
			std::wstring out;
			BYTE buf[256];
			DWORD len = 0;
			if (GetTokenInformation(token, TokenUser, buf, sizeof(buf), &len))
			{
				LPWSTR s = nullptr;
				if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &s))
				{
					out = s;
					LocalFree(s);
				}
			}
			CloseHandle(token);
			return out;
		}

		std::wstring LauncherExe()
		{
			wchar_t base[MAX_PATH];
			DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
			if (n == 0 || n >= MAX_PATH)
				return L"";
			return std::wstring(base) + L"\\MeowyClientPack\\launcher\\MeowyLauncher.exe";
		}
	}

	namespace detail
	{
		bool SameFile(const std::wstring& a, const std::wstring& b)
		{
			auto id = [](const std::wstring& path, BY_HANDLE_FILE_INFORMATION* info)
			{
				if (path.empty())
					return false;
				HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
				if (h == INVALID_HANDLE_VALUE)
					return false;
				const bool ok = GetFileInformationByHandle(h, info) != 0;
				CloseHandle(h);
				return ok;
			};
			BY_HANDLE_FILE_INFORMATION x = {}, y = {};
			return id(a, &x) && id(b, &y) && x.dwVolumeSerialNumber == y.dwVolumeSerialNumber
				&& x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
		}
	}

	namespace
	{
		// The pipe's server is the installed launcher (not a squatter).
		bool ServerIsLauncher(HANDLE pipe)
		{
			ULONG pid = 0;
			if (!GetNamedPipeServerProcessId(pipe, &pid))
				return false;
			HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (!p)
				return false;
			wchar_t path[MAX_PATH * 2];
			DWORD size = MAX_PATH * 2;
			// Windows reports the image's final path: with the profile behind a
			// junction only the file itself matches.
			const std::wstring image(path, QueryFullProcessImageNameW(p, 0, path, &size) ? size : 0);
			const std::wstring exe = LauncherExe();
			const bool ok = !image.empty() && (detail::SamePath(image, exe) || detail::SameFile(image, exe));
			CloseHandle(p);
			return ok;
		}

		bool WriteLine(HANDLE pipe, const std::string& line)
		{
			DWORD w = 0;
			return !line.empty() && WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &w, nullptr) && w == line.size();
		}

		// One connection, until it breaks.
		void Serve(HANDLE pipe)
		{
			if (!WriteLine(pipe, detail::HelloLine(g_version)))
				return;
			unsigned sent = 0;
			std::string pending;
			for (;;)
			{
				unsigned version = g_stateVersion;
				if (version != sent)
				{
					std::string state, address;
					{
						std::lock_guard<std::mutex> lock(g_lock);
						state = g_state;
						address = g_address;
					}
					if (!WriteLine(pipe, detail::StateLine(state, address)))
						return;
					sent = version;
				}
				DWORD avail = 0;
				if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr))
					return;
				if (avail > 0)
				{
					char buf[512];
					DWORD got = 0;
					if (!ReadFile(pipe, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, nullptr) || got == 0)
						return;
					pending.append(buf, got);
					size_t nl;
					while ((nl = pending.find('\n')) != std::string::npos)
					{
						std::string address;
						if (detail::ParseJoin(pending.substr(0, nl), address))
						{
							std::lock_guard<std::mutex> lock(g_lock);
							g_join = address;
						}
						pending.erase(0, nl + 1);
					}
					if (pending.size() > 512)
						return;   // not our protocol
				}
				Sleep(200);
			}
		}

		void Run()
		{
			const std::wstring sid = UserSid();
			if (sid.empty())
				return;
			const std::wstring name = L"\\\\.\\pipe\\meowy-launcher-" + sid;
			for (;;)
			{
				HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
					SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
				if (pipe != INVALID_HANDLE_VALUE)
				{
					if (ServerIsLauncher(pipe))
					{
						g_connected = true;
						Serve(pipe);
						g_connected = false;
					}
					CloseHandle(pipe);
				}
				Sleep(5000);
			}
		}
	}

	void Start(const char* version)
	{
		if (g_started.exchange(true))
			return;
		g_version = version ? version : "";
		std::thread(Run).detach();
	}
}

#else

namespace launcher_pipe
{
	void Start(const char*) {}
}

#endif
