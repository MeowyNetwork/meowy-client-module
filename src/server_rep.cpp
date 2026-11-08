#include "server_rep.h"
#include "steam_ticket.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace
{
	const wchar_t* const kHost = L"meowynetwork.com";
	const wchar_t* const kListPath = L"/api/pack/servers";
	const wchar_t* const kReportPath = L"/api/pack/servers/report";
	constexpr size_t kMaxBody = 512 * 1024;
	constexpr size_t kMaxCapture = 64 * 1024;
	// A2S_PLAYER: the header's count is one byte, so no honest reply lists more
	// than 255, and no Steam name is longer than 64 bytes. A hostile reply can
	// hold ~7,000 entries and names of any length, and the server list's checks
	// (and the side panel) would do work in proportion.
	constexpr int kMaxPlayers = 255;
	constexpr size_t kMaxName = 64;

	std::mutex g_listMutex;
	std::string g_list;
	std::atomic<bool> g_fetching{ false };

	std::mutex g_captureMutex;
	std::string g_capture;
	std::atomic<bool> g_capturing{ false };

	// %LOCALAPPDATA%\MeowyClientPack\serverlist.log: what the reports did (counts and
	// HTTP results only, never SteamIDs), overwritten past 64 KB.
	void Log(const std::string& line)
	{
		wchar_t base[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return;
		std::wstring path = std::wstring(base) + L"\\MeowyClientPack\\serverlist.log";
		WIN32_FILE_ATTRIBUTE_DATA info;
		bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 64 * 1024;
		FILE* f = _wfopen(path.c_str(), big ? L"w" : L"a");
		if (!f)
			return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(f, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, line.c_str());
		fclose(f);
	}

	// One HTTPS request to kHost. Returns the body for a 200, else "".
	std::string Request(const wchar_t* method, const wchar_t* path, const std::string& body, DWORD* statusOut = nullptr)
	{
		std::string out;
		HINTERNET session = WinHttpOpen(L"MeowyClientPack", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
			return out;
		WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
		HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
		HINTERNET request = connect ? WinHttpOpenRequest(connect, method, path, nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
		if (request)
		{
			// No redirects: the answer must come from kHost itself.
			DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
			WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
			const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
			BOOL sent = WinHttpSendRequest(request, headers, body.empty() ? 0 : (DWORD)-1L,
				body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), (DWORD)body.size(), (DWORD)body.size(), 0);
			if (sent && WinHttpReceiveResponse(request, nullptr))
			{
				DWORD status = 0, size = sizeof(status);
				WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
				if (statusOut)
					*statusOut = status;
				if (status == 200)
				{
					DWORD avail = 0;
					while (WinHttpQueryDataAvailable(request, &avail) && avail > 0 && out.size() < kMaxBody)
					{
						std::string chunk(avail, '\0');
						DWORD read = 0;
						if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0)
							break;
						out.append(chunk.data(), read);
					}
					if (out.size() >= kMaxBody)
						out.clear();
				}
			}
			WinHttpCloseHandle(request);
		}
		if (connect)
			WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		return out;
	}
}

namespace
{
	std::mutex g_playersMutex;
	std::string g_players; // finished results, comma-joined JSON objects
	std::atomic<int> g_queries{ 0 };
	constexpr int kMaxQueries = 8;

	bool ParsePublicAddress(const std::string& address, sockaddr_in& out)
	{
		unsigned a, b, c, d, port;
		char tail;
		if (sscanf_s(address.c_str(), "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &tail, 1) != 5)
			return false;
		if (a > 255 || b > 255 || c > 255 || d > 255 || port == 0 || port > 65535)
			return false;
		if (a == 10 || a == 127 || a == 0 || a >= 224 || (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31)
			|| (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127))
			return false;
		out = {};
		out.sin_family = AF_INET;
		out.sin_port = htons((u_short)port);
		out.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
		return true;
	}

	// Length of the valid UTF-8 sequence at s[i] (1-4), or 0 when the bytes there
	// aren't one (a stray continuation byte, an overlong form, a surrogate, past
	// U+10FFFF, or cut off by the end).
	size_t Utf8Length(const unsigned char* s, size_t i, size_t n)
	{
		const unsigned char c = s[i];
		size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
		if (len <= 1)
			return len;
		if (i + len > n)
			return 0;
		for (size_t k = 1; k < len; ++k)
			if ((s[i + k] & 0xC0) != 0x80)
				return 0;
		const unsigned char c1 = s[i + 1];
		if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90))
			return 0;
		return len;
	}

	// A JSON string. Server names come from other people's servers: bytes that
	// aren't valid UTF-8 become U+FFFD, so one bad name can't make the whole
	// array (every finished query, joined) unreadable to util.JSONToTable.
	void JsonEscape(std::string& out, const char* s, size_t n)
	{
		const unsigned char* u = reinterpret_cast<const unsigned char*>(s);
		out += '"';
		for (size_t i = 0; i < n;)
		{
			const unsigned char ch = u[i];
			const size_t len = Utf8Length(u, i, n);
			if (len == 0)
			{
				out += "\\ufffd";
				++i;
			}
			else if (len > 1)
			{
				out.append(s + i, len);
				i += len;
			}
			else
			{
				if (ch == '"' || ch == '\\') { out += '\\'; out += (char)ch; }
				else if (ch < 0x20 || ch == 0x7F) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", ch); out += buf; }
				else out += (char)ch;
				++i;
			}
		}
		out += '"';
	}

	// One reply, split packets joined. Empty on timeout or nonsense. The socket
	// is connected, so only the server's own datagrams arrive; a server feeding
	// split packets forever is still cut off by the deadline and the packet cap.
	std::string Receive(SOCKET s)
	{
		char buf[2048];
		std::string parts[32];
		int total = 0, have = 0, packets = 0;
		const ULONGLONG deadline = GetTickCount64() + 2500;
		for (;;)
		{
			if (GetTickCount64() > deadline || ++packets > 64)
				return "";
			int n = recv(s, buf, sizeof(buf), 0);
			if (n < 5)
				return "";
			int32_t kind;
			memcpy(&kind, buf, 4);
			if (kind == -1)
				return std::string(buf, n);
			if (kind != -2 || n < 12)
				return "";
			int count = (unsigned char)buf[8], index = (unsigned char)buf[9];
			if (count == 0 || count > 32 || index >= count)
				return "";
			total = count;
			if (parts[index].empty())
				++have;
			parts[index].assign(buf + 12, n - 12);
			if (have >= total)
			{
				std::string joined;
				for (int i = 0; i < total; ++i)
					joined += parts[i];
				return joined;
			}
		}
	}

	std::string QueryPlayersNow(const std::string& address)
	{
		std::string json = "{\"address\":";
		JsonEscape(json, address.data(), address.size());
		sockaddr_in to;
		SOCKET s = INVALID_SOCKET;
		std::string reply;
		bool answered = false;
		if (ParsePublicAddress(address, to) && (s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != INVALID_SOCKET)
		{
			DWORD timeout = 1500;
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
			// Connected: datagrams from anyone but the server are dropped.
			if (connect(s, (sockaddr*)&to, sizeof(to)) == 0)
			{
				char req[9] = { '\xff', '\xff', '\xff', '\xff', 'U', '\xff', '\xff', '\xff', '\xff' };
				// Up to two challenge rounds and one resend after a lost packet.
				int challenges = 0, resends = 0;
				while (challenges + resends < 4)
				{
					send(s, req, sizeof(req), 0);
					reply = Receive(s);
					if (reply.empty())
					{
						if (++resends > 1)
							break;
						continue;
					}
					answered = true;
					if (reply.size() >= 9 && reply[4] == 'A' && ++challenges <= 2)
					{
						memcpy(req + 5, reply.data() + 5, 4); // the challenge, then ask again
						reply.clear();
						continue;
					}
					break;
				}
			}
			closesocket(s);
		}
		// "answered": the server replied but gave no list (hidden), as opposed
		// to no answer at all (which says nothing about the server).
		if (reply.size() < 6 || reply[4] != 'D')
			return json + ",\"ok\":false,\"answered\":" + (answered ? "true" : "false") + "}";

		json += ",\"ok\":true,\"declared\":" + std::to_string((unsigned char)reply[5]) + ",\"players\":[";
		size_t i = 6;
		int entries = 0;
		while (i < reply.size())
		{
			++i; // index
			size_t end = reply.find('\0', i);
			if (end == std::string::npos || end + 9 > reply.size())
				break;
			if (entries >= kMaxPlayers)
				return json + "],\"truncated\":true}";
			int32_t score;
			float time;
			memcpy(&score, reply.data() + end + 1, 4);
			memcpy(&time, reply.data() + end + 5, 4);
			// Seconds connected. Converting NaN, an infinity or anything past an
			// int is undefined behaviour: 0 instead (as for anything negative).
			if (!(time >= 0.0f && time < 1.0e9f))
				time = 0.0f;
			// Names cut to kMaxName bytes, at a UTF-8 character boundary.
			size_t nameLen = end - i;
			if (nameLen > kMaxName)
			{
				nameLen = kMaxName;
				while (nameLen > 0 && ((unsigned char)reply[i + nameLen] & 0xC0) == 0x80)
					--nameLen;
			}
			if (entries++ > 0)
				json += ',';
			json += "{\"name\":";
			JsonEscape(json, reply.data() + i, nameLen);
			json += ",\"score\":" + std::to_string(score) + ",\"time\":" + std::to_string(static_cast<int>(time)) + "}";
			i = end + 9;
		}
		return json + "]}";
	}
}

namespace server_rep
{
	bool IsPublicAddress(const std::string& address)
	{
		sockaddr_in unused;
		return ParsePublicAddress(address, unused);
	}

	bool QueryPlayers(const std::string& address)
	{
		sockaddr_in to;
		if (g_queries.load() >= kMaxQueries || !ParsePublicAddress(address, to))
			return false;
		++g_queries;
		std::thread([address] {
			static std::once_flag wsa;
			std::call_once(wsa, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
			std::string result = QueryPlayersNow(address);
			{
				std::lock_guard<std::mutex> lock(g_playersMutex);
				if (!g_players.empty())
					g_players += ',';
				g_players += result;
			}
			--g_queries;
		}).detach();
		return true;
	}

	std::string TakePlayers()
	{
		std::lock_guard<std::mutex> lock(g_playersMutex);
		if (g_players.empty())
			return "";
		std::string out = "[" + g_players + "]";
		g_players.clear();
		return out;
	}

	void FetchList()
	{
		bool expected = false;
		if (!g_fetching.compare_exchange_strong(expected, true))
			return;
		std::thread([] {
			std::string body = Request(L"GET", kListPath, "");
			if (!body.empty())
			{
				std::lock_guard<std::mutex> lock(g_listMutex);
				g_list = std::move(body);
			}
			g_fetching = false;
		}).detach();
	}

	std::string TakeList()
	{
		std::lock_guard<std::mutex> lock(g_listMutex);
		std::string out;
		out.swap(g_list);
		return out;
	}

	void SendReport(const std::string& json)
	{
		if (json.empty())
			return;
		// The biggest servers (two runs of 128 rows) fit; past that, say so.
		if (json.size() > 64 * 1024)
		{
			Log("report dropped: " + std::to_string(json.size()) + " bytes");
			return;
		}
		// A Steam ticket for Meowy Network goes with it (steam_ticket.h), so the
		// report counts as one Steam account's. Steam answers through a callback
		// on the game's thread; the worker waits up to 10 s for it, and without
		// one the report goes as 1.7.0/1.7.1 sent theirs.
		steam_ticket::Pump();
		std::string why;
		const uint32_t handle = steam_ticket::Request(&why);
		std::thread([json, handle, why] {
			std::string whyNot = why;
			const std::string hex = handle ? steam_ticket::Wait(handle, 10000, &whyNot) : std::string();
			const std::string body = hex.empty() ? json : steam_ticket::detail::WithTicket(json, hex);
			DWORD status = 0;
			Request(L"POST", kReportPath, body, &status);
			steam_ticket::Release(handle);   // the backend has checked it by now; cancelled on the game thread
			Log("report sent, " + std::to_string(body.size()) + " bytes, HTTP " + std::to_string(status)
				+ (hex.empty() ? ", no Steam ticket (" + whyNot + ")" : ", with a Steam ticket"));
		}).detach();
	}

	void CaptureBegin()
	{
		std::lock_guard<std::mutex> lock(g_captureMutex);
		g_capture.clear();
		g_capturing = true;
	}

	std::string CaptureEnd()
	{
		std::lock_guard<std::mutex> lock(g_captureMutex);
		g_capturing = false;
		std::string out;
		out.swap(g_capture);
		return out;
	}

	void Note(const std::string& line)
	{
		Log(line);
	}

	std::string Exchange(const std::string& address, char kind, bool& answered)
	{
		answered = false;
		sockaddr_in to;
		std::string reply;
		SOCKET s = INVALID_SOCKET;
		if (!ParsePublicAddress(address, to) || (s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET)
			return reply;
		DWORD timeout = 1500;
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
		// Connected: datagrams from anyone but the server are dropped.
		if (connect(s, (sockaddr*)&to, sizeof(to)) == 0)
		{
			char req[9] = { '\xff', '\xff', '\xff', '\xff', kind, '\xff', '\xff', '\xff', '\xff' };
			// Up to two challenge rounds and one resend after a lost packet.
			int challenges = 0, resends = 0;
			while (challenges + resends < 4)
			{
				send(s, req, sizeof(req), 0);
				reply = Receive(s);
				if (reply.empty())
				{
					if (++resends > 1)
						break;
					continue;
				}
				answered = true;
				if (reply.size() >= 9 && reply[4] == 'A' && ++challenges <= 2)
				{
					memcpy(req + 5, reply.data() + 5, 4); // the challenge, then ask again
					reply.clear();
					continue;
				}
				break;
			}
		}
		closesocket(s);
		if (reply.size() >= 5 && reply[4] == 'A')
			reply.clear();   // still a challenge after two rounds
		return reply;
	}

	void Capture(const char* message)
	{
		if (!g_capturing || !message)
			return;
		std::lock_guard<std::mutex> lock(g_captureMutex);
		if (g_capture.size() < kMaxCapture)
			g_capture.append(message);
	}
}
