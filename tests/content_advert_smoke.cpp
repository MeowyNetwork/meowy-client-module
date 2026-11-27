// Content platform adverts and the server list's network side, no game:
//   content_advert_smoke [a.b.c.d:port]
// Reading the `meowy_content` rule out of A2S_RULES replies built here (a
// normal one, a missing rule, hostile ones); joining split A2S replies (parts
// of another reply dropped); A2S_PLAYER replies as typed results; net_http
// against a local HTTP stub on 127.0.0.1 (a whole body, one cut off halfway, a
// trickle past the deadline, keep-alive) and the server index through it (no
// ETag kept from a cut-off body); and thread spawns forced to fail leaving no
// flag or slot taken. The A2S queries after those failures go to 203.0.113.x
// (TEST-NET-3, never routed), which never answers. With an address, it also
// asks that server for real and prints what it found (not a pass/fail check:
// the server may have no advert). Exit code 0 when every check passes.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../src/content_advert.h"
#include "../src/net_http.h"
#include "../src/server_index.h"
#include "../src/server_rep.h"
#include "../src/thread_util.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0, g_count = 0;

static void Check(const char* name, bool ok)
{
	++g_count;
	if (!ok)
	{
		++g_failures;
		std::printf("FAIL %s\n", name);
	}
}

// An A2S_RULES reply: header, 16-bit count, then name\0value\0 pairs.
static std::string Reply(std::initializer_list<std::pair<std::string, std::string>> rules, int count = -1)
{
	std::string r("\xFF\xFF\xFF\xFF" "E", 5);
	const int n = count >= 0 ? count : static_cast<int>(rules.size());
	r += static_cast<char>(n & 0xFF);
	r += static_cast<char>((n >> 8) & 0xFF);
	for (const auto& kv : rules)
	{
		r += kv.first;
		r += '\0';
		r += kv.second;
		r += '\0';
	}
	return r;
}

// One part of a split A2S reply: -2, split ID, part count, part number, size, payload.
static std::string Part(int32_t id, int count, int index, const std::string& payload)
{
	std::string p("\xFE\xFF\xFF\xFF", 4);
	p.append(reinterpret_cast<const char*>(&id), 4);
	p += static_cast<char>(count);
	p += static_cast<char>(index);
	p += std::string("\xE0\x04", 2);
	return p + payload;
}

struct Player
{
	std::string name;
	int32_t score;
	float time;
};

// An A2S_PLAYER reply: header, 'D', the declared count, then the entries.
static std::string PlayerReply(int declared, const std::vector<Player>& players)
{
	std::string r("\xFF\xFF\xFF\xFF" "D", 5);
	r += static_cast<char>(declared);
	char index = 0;
	for (const Player& p : players)
	{
		r += index++;
		r += p.name;
		r += '\0';
		r.append(reinterpret_cast<const char*>(&p.score), 4);
		r.append(reinterpret_cast<const char*>(&p.time), 4);
	}
	return r;
}

static uint32_t Crc32(const std::string& s)
{
	uint32_t c = 0xFFFFFFFFu;
	for (unsigned char b : s)
	{
		c ^= b;
		for (int k = 0; k < 8; ++k)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

// `body` gzipped with stored (uncompressed) deflate blocks: a real gzip
// stream for WinHTTP to unpack, without a compressor here.
static std::string Gzip(const std::string& body)
{
	std::string z("\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\xFF", 10);
	size_t at = 0;
	do
	{
		const size_t n = (body.size() - at) < 65535 ? body.size() - at : 65535;
		const bool last = at + n == body.size();
		z += static_cast<char>(last ? 1 : 0);
		z += static_cast<char>(n & 0xFF);
		z += static_cast<char>((n >> 8) & 0xFF);
		z += static_cast<char>(~n & 0xFF);
		z += static_cast<char>((~n >> 8) & 0xFF);
		z.append(body, at, n);
		at += n;
	} while (at < body.size());
	const uint32_t crc = Crc32(body), size = static_cast<uint32_t>(body.size());
	z.append(reinterpret_cast<const char*>(&crc), 4);
	z.append(reinterpret_cast<const char*>(&size), 4);
	return z;
}

static const std::string kPlain = "{" + std::string(4000, 'z') + "}";

// --- A local HTTP stub on 127.0.0.1 (plain HTTP/1.1, a thread per connection) ---
namespace stub
{
	SOCKET g_listen = INVALID_SOCKET;
	unsigned short g_port = 0;
	std::atomic<int> g_accepts{ 0 };
	std::mutex g_lock;
	std::vector<std::string> g_requests;   // each request's head, in order
	std::string g_indexMode = "cut";       // what /api/pack/index answers: "cut" or "whole"

	bool SendAll(SOCKET s, const std::string& data)
	{
		size_t at = 0;
		while (at < data.size())
		{
			const int n = send(s, data.data() + at, static_cast<int>(data.size() - at), 0);
			if (n <= 0)
				return false;
			at += static_cast<size_t>(n);
		}
		return true;
	}

	// Answers one request; false closes the connection.
	bool Answer(SOCKET c, const std::string& path, const std::string& head)
	{
		if (path == "/whole")
			return SendAll(c, "HTTP/1.1 200 OK\r\nContent-Length: 3000\r\nETag: \"w1\"\r\nX-Meowy-Geo: 1.5,2.5\r\n\r\n" + std::string(3000, 'w'));
		if (path == "/cut")
		{
			// Half the body, then the connection closes (a clean close, not a reset).
			SendAll(c, "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n" + std::string(500, 'c'));
			Sleep(100);
			return false;
		}
		if (path == "/gzip" || path == "/gzcut")
		{
			const std::string z = Gzip(kPlain);
			const std::string headers = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " + std::to_string(z.size()) + "\r\n\r\n";
			if (path == "/gzip")
				return SendAll(c, headers + z);
			SendAll(c, headers + z.substr(0, z.size() / 2));
			Sleep(100);
			return false;
		}
		if (path == "/chunked" || path == "/chunkcut")
		{
			SendAll(c, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1f4\r\n" + std::string(500, 'k') + "\r\n");
			if (path == "/chunked")
				return SendAll(c, "1f4\r\n" + std::string(500, 'k') + "\r\n0\r\n\r\n");
			Sleep(100);
			return false;
		}
		if (path == "/trickle")
		{
			if (!SendAll(c, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n"))
				return false;
			for (int i = 0; i < 100; ++i)
			{
				if (!SendAll(c, "t"))
					return false;
				Sleep(100);
			}
			return true;
		}
		if (path == "/api/pack/index")
		{
			std::string mode;
			{
				std::lock_guard<std::mutex> lock(g_lock);
				mode = g_indexMode;
			}
			if (mode == "cut")
			{
				SendAll(c, "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nETag: \"cut1\"\r\n\r\n{" + std::string(499, 'x'));
				Sleep(100);
				return false;
			}
			if (head.find("If-None-Match: \"v1\"") != std::string::npos)
				return SendAll(c, "HTTP/1.1 304 Not Modified\r\nETag: \"v1\"\r\n\r\n");
			const std::string body = "{\"rows\":[]}";
			return SendAll(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nETag: \"v1\"\r\nContent-Length: "
				+ std::to_string(body.size()) + "\r\n\r\n" + body);
		}
		return SendAll(c, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
	}

	void Serve(SOCKET c)
	{
		DWORD timeout = 10000;
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
		std::string pending;
		for (;;)
		{
			size_t end;
			while ((end = pending.find("\r\n\r\n")) == std::string::npos)
			{
				char buf[4096];
				const int n = recv(c, buf, sizeof(buf), 0);
				if (n <= 0)
				{
					closesocket(c);
					return;
				}
				pending.append(buf, static_cast<size_t>(n));
			}
			const std::string head = pending.substr(0, end + 4);
			pending.erase(0, end + 4);
			{
				std::lock_guard<std::mutex> lock(g_lock);
				g_requests.push_back(head);
			}
			const size_t from = head.find(' ') + 1;
			const std::string path = head.substr(from, head.find(' ', from) - from);
			if (!Answer(c, path, head))
			{
				closesocket(c);
				return;
			}
		}
	}

	bool Start()
	{
		g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (g_listen == INVALID_SOCKET)
			return false;
		sockaddr_in at = {};
		at.sin_family = AF_INET;
		at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		at.sin_port = 0;
		int len = sizeof(at);
		if (bind(g_listen, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0 || listen(g_listen, 16) != 0
			|| getsockname(g_listen, reinterpret_cast<sockaddr*>(&at), &len) != 0)
			return false;
		g_port = ntohs(at.sin_port);
		std::thread([] {
			for (;;)
			{
				SOCKET c = accept(g_listen, nullptr, nullptr);
				if (c == INVALID_SOCKET)
					return;
				++g_accepts;
				std::thread(Serve, c).detach();
			}
		}).detach();
		return true;
	}

	std::string LastRequest()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_requests.empty() ? std::string() : g_requests.back();
	}

	net_http::Request Get(const wchar_t* path)
	{
		net_http::Request req;
		req.host = L"127.0.0.1";
		req.port = g_port;
		req.secure = false;
		req.path = path;
		req.maxBody = 64 * 1024;
		req.t.connect = 3000;
		req.t.send = 3000;
		req.t.receive = 5000;
		req.t.totalMs = 10000;
		return req;
	}
}

static server_index::Result WaitIndex()
{
	for (int i = 0; i < 300; ++i)
	{
		server_index::Result r = server_index::Take();
		if (r.ready)
			return r;
		Sleep(50);
	}
	return server_index::Result();
}

static void SplitChecks()
{
	using server_rep::detail::SplitJoin;
	std::string out;
	{
		SplitJoin j;
		Check("split: a single packet is whole", j.Add("\xFF\xFF\xFF\xFF" "Dxyz", 8, out) == 1 && out == std::string("\xFF\xFF\xFF\xFF" "Dxyz", 8));
	}
	{
		SplitJoin j;
		const std::string a = Part(7, 2, 1, "CD"), b = Part(7, 2, 0, "AB");
		Check("split: out of order, joined", j.Add(a.data(), a.size(), out) == 0 && j.Add(b.data(), b.size(), out) == 1 && out == "ABCD");
	}
	{
		// A late reply's parts (ID 9) arrive while the resent ask's (ID 7) come in.
		SplitJoin j;
		const std::string a0 = Part(7, 2, 0, "AB"), x1 = Part(9, 2, 1, "XX"), x0 = Part(9, 2, 0, "YY"), a1 = Part(7, 2, 1, "CD");
		const int r1 = j.Add(a0.data(), a0.size(), out), r2 = j.Add(x1.data(), x1.size(), out), r3 = j.Add(x0.data(), x0.size(), out);
		Check("split: another reply's parts dropped", r1 == 0 && r2 == 0 && r3 == 0);
		Check("split: never mixed with them", j.Add(a1.data(), a1.size(), out) == 1 && out == "ABCD");
	}
	{
		SplitJoin j;
		const std::string a0 = Part(7, 2, 0, "AB"), odd = Part(7, 3, 1, "ZZ"), a1 = Part(7, 2, 1, "CD");
		j.Add(a0.data(), a0.size(), out);
		Check("split: a part with another count dropped", j.Add(odd.data(), odd.size(), out) == 0);
		Check("split: the reply still joins", j.Add(a1.data(), a1.size(), out) == 1 && out == "ABCD");
	}
	{
		SplitJoin j;
		const std::string zero = Part(7, 0, 0, "AB"), past = Part(7, 2, 2, "AB"), many = Part(7, 33, 0, "AB");
		Check("split: no parts is nonsense", j.Add(zero.data(), zero.size(), out) == -1);
		Check("split: a part past the count is nonsense", j.Add(past.data(), past.size(), out) == -1);
		Check("split: over 32 parts is nonsense", j.Add(many.data(), many.size(), out) == -1);
		Check("split: too short is nonsense", j.Add("\xFE\xFF\xFF\xFF\x01", 5, out) == -1);
	}
}

static void PlayerChecks()
{
	using server_rep::detail::ParsePlayers;
	using server_rep::detail::PlayersJson;
	server_rep::PlayerResult r = ParsePlayers("203.0.113.5:27015",
		PlayerReply(3, { { "Alice", 12, 61.9f }, { "Bob", -3, 5.0f } }), true);
	Check("players: parsed", r.ok && r.answered && r.declared == 3 && r.players.size() == 2 && !r.truncated);
	Check("players: fields", r.players.size() == 2 && r.players[0].name == "Alice" && r.players[0].score == 12
		&& std::fabs(r.players[0].time - 61.9) < 0.01 && r.players[1].name == "Bob" && r.players[1].score == -3);
	Check("players: JSON as before", PlayersJson(r)
		== "{\"address\":\"203.0.113.5:27015\",\"ok\":true,\"declared\":3,\"players\":[{\"name\":\"Alice\",\"score\":12,\"time\":61},{\"name\":\"Bob\",\"score\":-3,\"time\":5}]}");

	r = ParsePlayers("203.0.113.5:27015", PlayerReply(1, { { std::string("A\xC3(\x01\"", 5), 0, std::numeric_limits<float>::quiet_NaN() } }), true);
	Check("players: bad UTF-8 becomes U+FFFD", r.players.size() == 1 && r.players[0].name == std::string("A\xEF\xBF\xBD(\x01\"", 7));
	Check("players: a NaN time is 0", r.players.size() == 1 && r.players[0].time == 0);
	Check("players: control and quote escaped in JSON", PlayersJson(r).find("\"name\":\"A\xEF\xBF\xBD(\\u0001\\\"\"") != std::string::npos);

	r = ParsePlayers("203.0.113.5:27015", PlayerReply(1, { { std::string(60, 'n') + "\xE2\x82\xAC\xE2\x82\xAC", 1, 1.0f } }), true);
	Check("players: a long name cut at a character, at most 64 bytes", r.players.size() == 1 && r.players[0].name == std::string(60, 'n') + "\xE2\x82\xAC");

	std::vector<Player> many(300, Player{ "p", 1, 1.0f });
	r = ParsePlayers("203.0.113.5:27015", PlayerReply(255, many), true);
	Check("players: at most 255, the rest dropped", r.players.size() == 255 && r.truncated && PlayersJson(r).find("],\"truncated\":true}") != std::string::npos);

	r = ParsePlayers("203.0.113.5:27015", "", false);
	Check("players: no answer", !r.ok && !r.answered && PlayersJson(r) == "{\"address\":\"203.0.113.5:27015\",\"ok\":false,\"answered\":false}");
	r = ParsePlayers("203.0.113.5:27015", std::string("\xFF\xFF\xFF\xFF" "I\x11", 6), true);
	Check("players: answered but no list", !r.ok && r.answered && PlayersJson(r).find("\"answered\":true") != std::string::npos);
	r = ParsePlayers("203.0.113.5:27015", PlayerReply(2, { { "cut", 1, 1.0f } }).substr(0, 12), true);
	Check("players: a cut-off entry isn't read", r.ok && r.players.empty());
}

static void HttpChecks()
{
	if (!stub::Start())
	{
		Check("http: the local stub starts", false);
		return;
	}
	net_http::Session& shared = net_http::Session::Shared();

	net_http::Request req = stub::Get(L"/whole");
	req.readHeader = L"X-Meowy-Geo";
	const int accepts = stub::g_accepts.load();
	net_http::Response r = shared.Send(req);
	Check("http: a whole body is complete", r.status == 200 && r.complete && r.body == std::string(3000, 'w') && r.error.empty());
	Check("http: its ETag and the asked-for header", r.etag == "\"w1\"" && r.header == "1.5,2.5");
	r = shared.Send(req);
	Check("http: the next request keeps the connection", r.complete && stub::g_accepts.load() == accepts + 1);

	req = stub::Get(L"/whole");
	req.maxBody = 1000;
	r = shared.Send(req);
	Check("http: over the cap is too big, not complete", r.status == 200 && !r.complete && r.error == "too big");

	r = shared.Send(stub::Get(L"/cut"));
	Check("http: a body cut off halfway isn't complete", r.status == 200 && !r.complete && r.error.compare(0, 7, "cut off") == 0);
	r = shared.Send(stub::Get(L"/gzip"));
	Check("http: a gzip body unpacked, complete", r.status == 200 && r.complete && r.body == kPlain);
	r = shared.Send(stub::Get(L"/gzcut"));
	Check("http: a gzip body cut off halfway isn't complete", r.status == 200 && !r.complete && r.error.compare(0, 7, "cut off") == 0);
	r = shared.Send(stub::Get(L"/chunked"));
	Check("http: a chunked body, complete", r.status == 200 && r.complete && r.body == std::string(1000, 'k'));
	r = shared.Send(stub::Get(L"/chunkcut"));
	Check("http: a chunked body cut off isn't complete", r.status == 200 && !r.complete && r.error.compare(0, 7, "cut off") == 0);

	req = stub::Get(L"/trickle");
	req.t.totalMs = 1000;
	const ULONGLONG start = GetTickCount64();
	r = shared.Send(req);
	const ULONGLONG took = GetTickCount64() - start;
	Check("http: a trickle past the total deadline is too slow", !r.complete && r.error == "too slow" && took < 3000);

	req = stub::Get(L"/whole");
	req.etag = "\"w0\"";
	{
		net_http::Session own = net_http::Session::Private();
		r = own.Send(req);
	}
	Check("http: a private session works, and sends If-None-Match", r.complete && stub::LastRequest().find("If-None-Match: \"w0\"") != std::string::npos);
	Check("http: gzip and deflate asked for", stub::LastRequest().find("Accept-Encoding: gzip, deflate") != std::string::npos);

	req = stub::Get(L"/whole");
	req.port = 1;   // nothing listens there
	req.t.connect = 1000;
	r = shared.Send(req);
	Check("http: no answer", r.status == 0 && !r.complete && r.error.compare(0, 9, "no answer") == 0);

	// The server index through the stub.
	server_index::detail::SetEndpoint(L"127.0.0.1", stub::g_port, false);
	thread_util::FailNextSpawns(1);
	Check("index: a failed spawn is no fetch", !server_index::Fetch());
	Check("index: and leaves no fetch running", server_index::Describe().find("(fetching)") == std::string::npos);
	Check("index: the next one starts", server_index::Fetch());
	server_index::Result ir = WaitIndex();
	Check("index: a cut-off body is an error", ir.ready && ir.status.compare(0, 14, "error: cut off") == 0 && ir.body.empty());
	{
		std::lock_guard<std::mutex> lock(stub::g_lock);
		stub::g_indexMode = "whole";
	}
	server_index::Fetch();
	ir = WaitIndex();
	Check("index: no ETag kept from the cut-off body", stub::LastRequest().find("If-None-Match") == std::string::npos);
	Check("index: a whole body is ok", ir.status == "ok" && ir.body == "{\"rows\":[]}");
	server_index::Fetch();
	ir = WaitIndex();
	Check("index: its ETag is sent next time, and 304 is same", stub::LastRequest().find("If-None-Match: \"v1\"") != std::string::npos
		&& ir.status == "same");
}

// Spawns forced to fail give back what they took: the A2S asks afterwards go
// to TEST-NET-3 (never routed, never answers).
static void SpawnFailureChecks()
{
	for (int i = 0; i < 9; ++i)
	{
		thread_util::FailNextSpawns(1);
		server_rep::QueryPlayers("203.0.113." + std::to_string(i + 1) + ":27015");
	}
	thread_util::FailNextSpawns(0);
	Check("players: nine failed spawns leave every slot free", server_rep::QueryPlayers("203.0.113.20:27015"));

	thread_util::FailNextSpawns(1);
	Check("advert: a failed spawn is not asked", !content_advert::Ask("203.0.113.30:27015"));
	thread_util::FailNextSpawns(0);
	Check("advert: the same address can be asked again", content_advert::Ask("203.0.113.30:27015"));

	bool gotPlayers = false, gotAdvert = false;
	for (int i = 0; i < 100 && !(gotPlayers && gotAdvert); ++i)
	{
		Sleep(100);
		for (const server_rep::PlayerResult& p : server_rep::TakePlayerResults())
			if (p.address == "203.0.113.20:27015")
				gotPlayers = !p.ok && !p.answered;
		for (const content_advert::Answer& a : content_advert::Take())
			if (a.address == "203.0.113.30:27015")
				gotAdvert = !a.answered && !a.found;
	}
	Check("players: the query after the failures ran (no answer)", gotPlayers);
	Check("advert: the ask after the failure ran (no answer)", gotAdvert);
}

int main(int argc, char** argv)
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	using content_advert::detail::RuleValue;
	const char* rule = content_advert::kRule;
	std::string v;

	Check("found", RuleValue(Reply({ { "sv_gravity", "600" }, { "meowy_content", "1;ws=1,2" }, { "tv_enable", "0" } }), rule, v)
		&& v == "1;ws=1,2");
	Check("first rule", RuleValue(Reply({ { "meowy_content", "1;ws=5" } }), rule, v) && v == "1;ws=5");
	Check("missing", !RuleValue(Reply({ { "sv_gravity", "600" } }), rule, v) && v.empty());
	Check("a longer name isn't it", !RuleValue(Reply({ { "meowy_content_x", "1;ws=5" } }), rule, v));
	Check("a shorter name isn't it", !RuleValue(Reply({ { "meowy_conten", "1;ws=5" } }), rule, v));
	Check("empty value", !RuleValue(Reply({ { "meowy_content", "" } }), rule, v));
	Check("control character", !RuleValue(Reply({ { "meowy_content", std::string("1;ws=5\n") } }), rule, v));
	Check("high byte", !RuleValue(Reply({ { "meowy_content", std::string("1;ws=5\xC3\xA9") } }), rule, v));
	Check("too long", !RuleValue(Reply({ { "meowy_content", std::string(content_advert::kMaxValue + 1, '1') } }), rule, v));
	Check("longest", RuleValue(Reply({ { "meowy_content", std::string(content_advert::kMaxValue, '1') } }), rule, v)
		&& v.size() == content_advert::kMaxValue);
	Check("count larger than the pairs", !RuleValue(Reply({ { "sv_gravity", "600" } }, 50), rule, v));
	Check("count smaller: the rest isn't read", !RuleValue(Reply({ { "a", "1" }, { "meowy_content", "1;ws=5" } }, 1), rule, v));
	Check("not a rules reply", !RuleValue(std::string("\xFF\xFF\xFF\xFF" "D\x01\x00", 7), rule, v));
	Check("a challenge", !RuleValue(std::string("\xFF\xFF\xFF\xFF" "A\x01\x02\x03\x04", 9), rule, v));
	Check("too short", !RuleValue(std::string("\xFF\xFF\xFF\xFF" "E\x01", 6), rule, v));
	std::string cut = Reply({ { "meowy_content", "1;ws=5" } });
	cut.resize(cut.size() - 1);   // no NUL after the value
	Check("cut off", !RuleValue(cut, rule, v));

	Check("LAN address not asked", !content_advert::Ask("192.168.1.5:27015"));
	Check("a name not asked", !content_advert::Ask("example.com:27015"));

	SplitChecks();
	PlayerChecks();
	HttpChecks();
	SpawnFailureChecks();

	if (argc > 1)
	{
		const std::string address = argv[1];
		if (!content_advert::Ask(address))
		{
			std::printf("%s: not asked (not a public a.b.c.d:port?)\n", address.c_str());
		}
		else
		{
			for (int i = 0; i < 100; ++i)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				const auto answers = content_advert::Take();
				if (answers.empty())
					continue;
				for (const auto& a : answers)
					std::printf("%s: answered=%d found=%d value=%s\n", a.address.c_str(), a.answered ? 1 : 0, a.found ? 1 : 0,
						a.value.c_str());
				break;
			}
			std::printf("%s\n", content_advert::Describe().c_str());
		}
	}

	std::printf("%d checks, %d failed\n", g_count, g_failures);
	return g_failures == 0 ? 0 : 1;
}
