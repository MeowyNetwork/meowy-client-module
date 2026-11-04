#include "server_rep.h"

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

	std::mutex g_listMutex;
	std::string g_list;
	std::atomic<bool> g_fetching{ false };

	std::mutex g_captureMutex;
	std::string g_capture;
	std::atomic<bool> g_capturing{ false };

	// One HTTPS request to kHost. Returns the body for a 200, else "".
	std::string Request(const wchar_t* method, const wchar_t* path, const std::string& body)
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

	void JsonEscape(std::string& out, const char* s, size_t n)
	{
		out += '"';
		for (size_t i = 0; i < n; ++i)
		{
			unsigned char ch = (unsigned char)s[i];
			if (ch == '"' || ch == '\\') { out += '\\'; out += (char)ch; }
			else if (ch < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", ch); out += buf; }
			else out += (char)ch;
		}
		out += '"';
	}

	// One reply, split packets joined. Empty on timeout or nonsense.
	std::string Receive(SOCKET s)
	{
		char buf[2048];
		std::string parts[32];
		int total = 0, have = 0;
		for (;;)
		{
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
		if (ParsePublicAddress(address, to) && (s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != INVALID_SOCKET)
		{
			DWORD timeout = 2500;
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
			char req[9] = { '\xff', '\xff', '\xff', '\xff', 'U', '\xff', '\xff', '\xff', '\xff' };
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				sendto(s, req, sizeof(req), 0, (sockaddr*)&to, sizeof(to));
				reply = Receive(s);
				if (reply.size() >= 9 && reply[4] == 'A')
				{
					memcpy(req + 5, reply.data() + 5, 4); // the challenge, then ask again
					reply.clear();
					continue;
				}
				break;
			}
			closesocket(s);
		}
		if (reply.size() < 6 || reply[4] != 'D')
			return json + ",\"ok\":false}";

		json += ",\"ok\":true,\"declared\":" + std::to_string((unsigned char)reply[5]) + ",\"players\":[";
		size_t i = 6;
		bool first = true;
		while (i < reply.size())
		{
			++i; // index
			size_t end = reply.find('\0', i);
			if (end == std::string::npos || end + 9 > reply.size())
				break;
			int32_t score;
			float time;
			memcpy(&score, reply.data() + end + 1, 4);
			memcpy(&time, reply.data() + end + 5, 4);
			if (!first)
				json += ',';
			first = false;
			json += "{\"name\":";
			JsonEscape(json, reply.data() + i, end - i);
			json += ",\"score\":" + std::to_string(score) + ",\"time\":" + std::to_string((int)time) + "}";
			i = end + 9;
		}
		return json + "]}";
	}
}

namespace server_rep
{
	void QueryPlayers(const std::string& address)
	{
		if (g_queries.load() >= kMaxQueries)
			return;
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
		if (json.empty() || json.size() > 16 * 1024)
			return;
		std::thread([json] { Request(L"POST", kReportPath, json); }).detach();
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

	void Capture(const char* message)
	{
		if (!g_capturing || !message)
			return;
		std::lock_guard<std::mutex> lock(g_captureMutex);
		if (g_capture.size() < kMaxCapture)
			g_capture.append(message);
	}
}
