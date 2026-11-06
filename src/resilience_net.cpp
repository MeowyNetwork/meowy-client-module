// Connection resilience: is our server up, and is Steam online? (resilience.h)
//
// The rejoin's "is the server back?" question is the pack's own A2S_INFO
// query over plain UDP, not serverlist.PingServer: that goes through the
// Steam client, which may itself be offline after the same network drop
// (seen in a live test: "CClientSteamContext OnSteamServersDisconnected").
// It only ever asks one of our servers (server_gate::IsOurAddress).
#include "resilience.h"
#include "server_gate.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace
{
	std::atomic<int> g_probe{ 0 };       // 0 idle/pending, 1 answered, -1 no answer
	std::atomic<bool> g_running{ false };
	std::atomic<bool> g_locked{ false };

	bool ParseAddress(const std::string& address, sockaddr_in& out)
	{
		size_t colon = address.rfind(':');
		if (colon == std::string::npos)
			return false;
		std::string ip = address.substr(0, colon);
		int port = std::atoi(address.c_str() + colon + 1);
		if (port <= 0 || port > 65535)
			return false;
		std::memset(&out, 0, sizeof(out));
		out.sin_family = AF_INET;
		out.sin_port = htons(static_cast<u_short>(port));
		return inet_pton(AF_INET, ip.c_str(), &out.sin_addr) == 1;
	}

	// Skips a NUL-terminated string at buf[pos]; false if it runs off the end.
	bool SkipString(const unsigned char* buf, int len, int& pos)
	{
		while (pos < len && buf[pos])
			++pos;
		if (pos >= len)
			return false;
		++pos;
		return true;
	}

	// One A2S_INFO exchange (with the challenge round newer servers ask for).
	// 1 answered, -1 not; `locked` from the reply's visibility byte.
	int AskInfo(const sockaddr_in& to, bool& locked)
	{
		static std::once_flag wsa;
		std::call_once(wsa, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET)
			return -1;
		DWORD timeout = 2500;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
		int result = -1;
		unsigned char query[64];
		const char head[] = "\xFF\xFF\xFF\xFFTSource Engine Query";
		int qlen = static_cast<int>(sizeof(head));   // includes the terminating NUL
		std::memcpy(query, head, qlen);
		for (int round = 0; round < 3 && result < 0; ++round)
		{
			if (sendto(s, reinterpret_cast<const char*>(query), qlen, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR)
				break;
			unsigned char buf[1400];
			sockaddr_in from{};
			int fromLen = sizeof(from);
			int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
			if (n < 5 || from.sin_addr.s_addr != to.sin_addr.s_addr || from.sin_port != to.sin_port)
				continue;   // nothing, or not from the server we asked
			if (buf[0] != 0xFF || buf[1] != 0xFF || buf[2] != 0xFF || buf[3] != 0xFF)
				continue;
			if (buf[4] == 'A' && n >= 9)
			{
				// Challenge: ask again with it appended.
				std::memcpy(query + sizeof(head), buf + 5, 4);
				qlen = static_cast<int>(sizeof(head)) + 4;
				continue;
			}
			if (buf[4] == 'I')
			{
				int pos = 6;   // header, 'I', protocol
				bool ok = SkipString(buf, n, pos) && SkipString(buf, n, pos) && SkipString(buf, n, pos) && SkipString(buf, n, pos);
				pos += 2 + 1 + 1 + 1 + 1 + 1;   // app id, players, max, bots, type, environment
				locked = ok && pos < n && buf[pos] == 1;
				result = 1;
			}
		}
		closesocket(s);
		return result;
	}
}

namespace resilience
{
	bool StartProbe(const std::string& address)
	{
		sockaddr_in to;
		if (!server_gate::IsOurAddress(address) || !ParseAddress(address, to))
			return false;
		bool expected = false;
		if (!g_running.compare_exchange_strong(expected, true))
			return true;   // one already on its way
		g_probe = 0;
		g_locked = false;
		std::thread([to]() {
			bool locked = false;
			int r = AskInfo(to, locked);
			g_locked = locked;
			g_probe = r;
			g_running = false;
		}).detach();
		return true;
	}

	int ProbeResult(bool& locked)
	{
		if (g_running)
			return 0;
		int r = g_probe.exchange(0);
		locked = g_locked;
		return r;
	}

	int SteamOnline()
	{
#ifdef _WIN64
		HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
#else
		HMODULE steam = GetModuleHandleW(L"steam_api.dll");
#endif
		if (!steam)
			return -1;
		using GetUserFn = void* (__cdecl*)();
		using LoggedOnFn = bool (__cdecl*)(void* self);
		static const char* const versions[] = { "SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021", "SteamAPI_SteamUser_v020" };
		GetUserFn getUser = nullptr;
		for (const char* v : versions)
			if (!getUser)
				getUser = reinterpret_cast<GetUserFn>(GetProcAddress(steam, v));
		auto loggedOn = reinterpret_cast<LoggedOnFn>(GetProcAddress(steam, "SteamAPI_ISteamUser_BLoggedOn"));
		if (!getUser || !loggedOn)
			return -1;
		void* user = getUser();
		if (!user)
			return -1;
		return loggedOn(user) ? 1 : 0;
	}
}

#else

namespace resilience
{
	bool StartProbe(const std::string&) { return false; }
	int ProbeResult(bool& locked) { locked = false; return -1; }
	int SteamOnline() { return -1; }
}

#endif
