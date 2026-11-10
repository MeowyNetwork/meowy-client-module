#include "server_rep.h"
#include "steam_ticket.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "net_http.h"
#include "thread_util.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

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

	// One writer at a time: the game's thread and report threads both log.
	std::mutex g_logMutex;

	// %LOCALAPPDATA%\MeowyClientPack\serverlist.log: what the reports did (counts and
	// HTTP results only, never SteamIDs), overwritten past 64 KB.
	void Log(const std::string& line)
	{
		wchar_t base[MAX_PATH];
		DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return;
		std::wstring path = std::wstring(base) + L"\\MeowyClientPack\\serverlist.log";
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::lock_guard<std::mutex> lock(g_logMutex);
		WIN32_FILE_ATTRIBUTE_DATA info;
		bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 64 * 1024;
		FILE* f = _wfopen(path.c_str(), big ? L"w" : L"a");
		if (!f)
			return;
		fprintf(f, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, line.c_str());
		fclose(f);
	}

	// One HTTPS request to kHost (net_http's shared session). Returns the body
	// of a 200 that arrived whole, else "".
	std::string Request(const wchar_t* method, const wchar_t* path, const std::string& body, DWORD* statusOut = nullptr)
	{
		net_http::Request req;
		req.verb = method;
		req.host = kHost;
		req.path = path;
		req.body = body;
		if (!body.empty())
			req.headers = L"Content-Type: application/json\r\n";
		req.maxBody = kMaxBody;
		req.t.resolve = 5000;
		req.t.connect = 5000;
		req.t.send = 8000;
		req.t.receive = 8000;
		req.t.totalMs = 30000;
		net_http::Response resp = net_http::Session::Shared().Send(req);
		if (statusOut)
			*statusOut = resp.status;
		if (resp.status != 200 || !resp.complete)
			return std::string();
		return std::move(resp.body);
	}
}

namespace
{
	std::mutex g_playersMutex;
	std::vector<server_rep::PlayerResult> g_players;   // finished queries, waiting for a Take
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

	// Server names come from other people's servers: bytes that aren't valid
	// UTF-8 become U+FFFD, so one bad name can't make a whole batch unreadable
	// (util.JSONToTable, or Lua strings shown in the menu).
	std::string CleanUtf8(const char* s, size_t n)
	{
		const unsigned char* u = reinterpret_cast<const unsigned char*>(s);
		std::string out;
		out.reserve(n);
		for (size_t i = 0; i < n;)
		{
			const size_t len = Utf8Length(u, i, n);
			if (len == 0)
			{
				out += "\xEF\xBF\xBD";
				++i;
			}
			else
			{
				out.append(s + i, len);
				i += len;
			}
		}
		return out;
	}

	// A JSON string of valid UTF-8 (CleanUtf8's output, or ASCII).
	void JsonEscape(std::string& out, const std::string& s)
	{
		out += '"';
		for (char c : s)
		{
			const unsigned char ch = static_cast<unsigned char>(c);
			if (ch == '"' || ch == '\\') { out += '\\'; out += c; }
			else if (ch < 0x20 || ch == 0x7F) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", ch); out += buf; }
			else out += c;
		}
		out += '"';
	}

	// One reply, split packets joined. Empty on timeout or nonsense. The socket
	// is connected, so only the server's own datagrams arrive; a server feeding
	// split packets forever is still cut off by the deadline and the packet cap.
	std::string Receive(SOCKET s)
	{
		char buf[2048];
		server_rep::detail::SplitJoin join;
		std::string reply;
		int packets = 0;
		const ULONGLONG deadline = GetTickCount64() + 2500;
		for (;;)
		{
			if (GetTickCount64() > deadline || ++packets > 64)
				return "";
			int n = recv(s, buf, sizeof(buf), 0);
			if (n < 5)
				return "";
			const int got = join.Add(buf, static_cast<size_t>(n), reply);
			if (got > 0)
				return reply;
			if (got < 0)
				return "";
		}
	}

	server_rep::PlayerResult QueryPlayersNow(const std::string& address)
	{
		bool answered = false;
		const std::string reply = server_rep::Exchange(address, 'U', answered);
		return server_rep::detail::ParsePlayers(address, reply, answered);
	}
}

namespace server_rep
{
	namespace detail
	{
		int SplitJoin::Add(const char* data, size_t n, std::string& out)
		{
			if (n < 5)
				return -1;
			int32_t kind;
			memcpy(&kind, data, 4);
			if (kind == -1)
			{
				out.assign(data, n);
				return 1;
			}
			if (kind != -2 || n < 12)
				return -1;
			int32_t id;
			memcpy(&id, data + 4, 4);
			const int count = (unsigned char)data[8], index = (unsigned char)data[9];
			if (count == 0 || count > kMaxParts || index >= count)
				return -1;
			if (m_total == 0)
			{
				// The first part seen says which reply this is, and how many parts it has.
				m_id = id;
				m_total = count;
			}
			else if (id != m_id || count != m_total)
				return 0;   // part of another reply (a late one to an earlier ask): dropped
			if (m_parts[index].empty())
				++m_have;
			m_parts[index].assign(data + 12, n - 12);
			if (m_have < m_total)
				return 0;
			out.clear();
			for (int i = 0; i < m_total; ++i)
				out += m_parts[i];
			return 1;
		}

		PlayerResult ParsePlayers(const std::string& address, const std::string& reply, bool answered)
		{
			PlayerResult r;
			r.address = address;
			r.answered = answered;
			// "answered" without ok: the server replied but gave no list (hidden),
			// as opposed to no answer at all (which says nothing about the server).
			if (reply.size() < 6 || reply[4] != 'D')
				return r;
			r.ok = true;
			r.answered = true;
			r.declared = (unsigned char)reply[5];
			size_t i = 6;
			while (i < reply.size())
			{
				++i; // index
				size_t end = reply.find('\0', i);
				if (end == std::string::npos || end + 9 > reply.size())
					break;
				if (r.players.size() >= static_cast<size_t>(kMaxPlayers))
				{
					r.truncated = true;
					break;
				}
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
				PlayerResult::P p;
				p.name = CleanUtf8(reply.data() + i, nameLen);
				p.score = score;
				p.time = time;
				r.players.push_back(std::move(p));
				i = end + 9;
			}
			return r;
		}

		std::string PlayersJson(const PlayerResult& r)
		{
			std::string json = "{\"address\":";
			JsonEscape(json, CleanUtf8(r.address.data(), r.address.size()));
			if (!r.ok)
				return json + ",\"ok\":false,\"answered\":" + (r.answered ? "true" : "false") + "}";
			json += ",\"ok\":true,\"declared\":" + std::to_string(r.declared) + ",\"players\":[";
			for (size_t k = 0; k < r.players.size(); ++k)
			{
				const PlayerResult::P& p = r.players[k];
				if (k > 0)
					json += ',';
				json += "{\"name\":";
				JsonEscape(json, p.name);
				json += ",\"score\":" + std::to_string(p.score) + ",\"time\":" + std::to_string(static_cast<int>(p.time)) + "}";
			}
			return json + (r.truncated ? "],\"truncated\":true}" : "]}");
		}
	}

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
		const bool started = thread_util::Spawn(L"Meowy players", [address] {
			// The slot is given back however the body ends (an exception too).
			thread_util::OnExit done([] { --g_queries; });
			static std::once_flag wsa;
			std::call_once(wsa, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
			PlayerResult result = QueryPlayersNow(address);
			std::lock_guard<std::mutex> lock(g_playersMutex);
			g_players.push_back(std::move(result));
		}, thread_util::Priority::BelowNormal);
		if (!started)
			--g_queries;
		return started;
	}

	std::string TakePlayers()
	{
		std::vector<PlayerResult> done = TakePlayerResults();
		if (done.empty())
			return "";
		std::string out = "[";
		for (size_t k = 0; k < done.size(); ++k)
		{
			if (k > 0)
				out += ',';
			out += detail::PlayersJson(done[k]);
		}
		return out + "]";
	}

	std::vector<PlayerResult> TakePlayerResults()
	{
		std::lock_guard<std::mutex> lock(g_playersMutex);
		std::vector<PlayerResult> out;
		out.swap(g_players);
		return out;
	}

	void FetchList()
	{
		bool expected = false;
		if (!g_fetching.compare_exchange_strong(expected, true))
			return;
		if (!thread_util::Spawn(L"Meowy server list", [] {
				thread_util::OnExit done([] { g_fetching = false; });
				std::string body = Request(L"GET", kListPath, "");
				if (!body.empty())
				{
					std::lock_guard<std::mutex> lock(g_listMutex);
					g_list = std::move(body);
				}
			}, thread_util::Priority::BelowNormal))
			g_fetching = false;   // tried again at the next call
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
		const bool started = thread_util::Spawn(L"Meowy report", [json, handle, why] {
			// The ticket is given back however the body ends; the backend has
			// checked it by then. Cancelled on the game thread.
			thread_util::OnExit release([handle] { steam_ticket::Release(handle); });
			std::string whyNot = why;
			const std::string hex = handle ? steam_ticket::Wait(handle, 10000, &whyNot) : std::string();
			const std::string body = hex.empty() ? json : steam_ticket::detail::WithTicket(json, hex);
			DWORD status = 0;
			Request(L"POST", kReportPath, body, &status);
			Log("report sent, " + std::to_string(body.size()) + " bytes, HTTP " + std::to_string(status)
				+ (hex.empty() ? ", no Steam ticket (" + whyNot + ")" : ", with a Steam ticket"));
		}, thread_util::Priority::BelowNormal);
		if (!started)
		{
			steam_ticket::Release(handle);   // just asked for: never used
			Log("report not sent: no thread for it");
		}
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
