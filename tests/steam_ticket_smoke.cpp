// The server-list report's Steam ticket (src/steam_ticket.h).
//   steam_ticket_smoke                              the rules, offline (CI)
//   steam_ticket_smoke --live <GarrysMod folder>    a real ticket from the Steam client running on this
//                                                   PC, through the game's own steam_api (Steam shows you
//                                                   in Garry's Mod for a moment)
// With --live and STEAM_API_KEY set, Steam's Web API is also asked whose ticket it is, exactly as the
// backend asks (ISteamUserAuth/AuthenticateUserTicket, appid 4000, identity "meowynetwork"); that says
// whether the backend's key can check Garry's Mod tickets. The key and the ticket are never printed.
// Exit code 0 when every check passes.
#include "../src/steam_ticket.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

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

static int Offline()
{
	using namespace steam_ticket;
	const uint8_t bytes[] = { 0x00, 0xab, 0xff, 0x10 };
	Check("hex", detail::Hex(bytes, 4) == "00abff10");
	Check("hex: nothing", detail::Hex(bytes, 0).empty());

	// Into the report the menu Lua built (util.TableToJSON).
	Check("added to the report", detail::WithTicket("{\"address\":\"1.2.3.4:27015\",\"runs\":[]}", "abcd")
		== "{\"address\":\"1.2.3.4:27015\",\"runs\":[],\"ticket\":\"abcd\"}");
	Check("added to an empty object", detail::WithTicket("{}", "abcd") == "{\"ticket\":\"abcd\"}");
	Check("added before trailing space", detail::WithTicket("{\"a\":1}\n", "ab") == "{\"a\":1,\"ticket\":\"ab\"}\n");
	Check("not an object: unchanged", detail::WithTicket("[1]", "ab") == "[1]");
	Check("no ticket: unchanged", detail::WithTicket("{\"a\":1}", "") == "{\"a\":1}");
	Check("not plain hex: unchanged", detail::WithTicket("{\"a\":1}", "ab\"}") == "{\"a\":1}");
	Check("upper case: unchanged (Hex is lower case)", detail::WithTicket("{\"a\":1}", "AB") == "{\"a\":1}");
	Check("half a byte: unchanged", detail::WithTicket("{\"a\":1}", "abc") == "{\"a\":1}");
	Check("past Steam's size: unchanged", detail::WithTicket("{\"a\":1}", std::string(kMaxTicket * 2 + 2, 'a')) == "{\"a\":1}");

	// Steam answers on the game's thread while the report's worker waits.
	detail::Expect(5);
	std::thread steam([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		detail::Arrived(5, 1, bytes, 4);
	});
	std::string why;
	const auto t0 = std::chrono::steady_clock::now();
	Check("the worker gets the ticket", Wait(5, 5000, &why) == "00abff10");
	Check("as soon as it's there", std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
	steam.join();
	Check("taken once", Wait(5, 10, &why).empty() && why == "not asked for");

	detail::Expect(6);
	detail::Arrived(6, 2, nullptr, 0);   // k_EResultFail
	Check("Steam refused: no ticket", Wait(6, 1000, &why).empty() && why == "Steam answered 2");

	detail::Expect(7);
	Check("no answer: no ticket after the wait", Wait(7, 100, &why).empty() && why == "Steam didn't answer in time");
	detail::Arrived(7, 1, bytes, 4);   // too late: dropped
	detail::Arrived(99, 1, bytes, 4);  // never asked for: dropped
	Check("late or unknown answers dropped", Wait(7, 10, &why).empty() && Wait(99, 10, &why).empty());

	static uint8_t big[kMaxTicket + 1] = {};
	detail::Expect(8);
	detail::Arrived(8, 1, big, kMaxTicket + 1);
	Check("past Steam's size: no ticket", Wait(8, 1000, &why).empty());
	detail::Expect(9);
	detail::Arrived(9, 1, big, kMaxTicket);
	Check("Steam's full size: taken", Wait(9, 1000, &why).size() == static_cast<size_t>(kMaxTicket) * 2);

	// Released from the worker, cancelled on the game thread.
	Release(5);
	Release(0);
	Check("released: waiting for the game thread", detail::PendingCancels() == 1);
	Pump();
	Check("pumped: none waiting", detail::PendingCancels() == 0);

	// No steam_api in this process: nothing asked.
	Check("no Steam: no handle", Request(&why) == 0 && why == "Steam's API isn't loaded");

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}

// GET https://api.steampowered.com/<path>, the body or "" (and the status).
static std::string HttpsGet(const std::wstring& path, DWORD& status)
{
	std::string out;
	status = 0;
	HINTERNET session = WinHttpOpen(L"steam_ticket_smoke", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, L"api.steampowered.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
	{
		DWORD size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		DWORD avail = 0;
		while (WinHttpQueryDataAvailable(request, &avail) && avail > 0 && out.size() < 65536)
		{
			std::string chunk(avail, '\0');
			DWORD read = 0;
			if (!WinHttpReadData(request, &chunk[0], avail, &read) || read == 0)
				break;
			out.append(chunk.data(), read);
		}
	}
	if (request) WinHttpCloseHandle(request);
	if (connect) WinHttpCloseHandle(connect);
	if (session) WinHttpCloseHandle(session);
	return out;
}

static int Live(const char* garrysmod)
{
	const bool x64 = sizeof(void*) == 8;
	const std::string dll = std::string(garrysmod) + (x64 ? "\\bin\\win64\\steam_api64.dll" : "\\bin\\steam_api.dll");
	SetEnvironmentVariableA("SteamAppId", "4000");
	SetEnvironmentVariableA("SteamGameId", "4000");
	HMODULE steam = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!steam)
	{
		std::printf("couldn't load %s\n", dll.c_str());
		return 2;
	}
	// SteamAPI_InitFlat (SDK 1.58+): ESteamAPIInitResult, 0 = OK, and Steam's own words.
	using InitFn = int (__cdecl*)(char* errMsg);
	using VoidFn = void (__cdecl*)();
	using AccessorFn = void* (__cdecl*)();
	using SelfFn = uint64_t (__cdecl*)(void*);
	auto init = reinterpret_cast<InitFn>(GetProcAddress(steam, "SteamAPI_InitFlat"));
	auto run = reinterpret_cast<VoidFn>(GetProcAddress(steam, "SteamAPI_RunCallbacks"));
	auto shutdown = reinterpret_cast<VoidFn>(GetProcAddress(steam, "SteamAPI_Shutdown"));
	auto user = reinterpret_cast<AccessorFn>(GetProcAddress(steam, "SteamAPI_SteamUser_v023"));
	auto self = reinterpret_cast<SelfFn>(GetProcAddress(steam, "SteamAPI_ISteamUser_GetSteamID"));
	char err[1024] = {};
	const int initResult = init ? init(err) : -1;
	if (!run || !shutdown || !user || !self || initResult != 0)
	{
		std::printf("Steam didn't start for Garry's Mod (%d: %s). Is Steam running and signed in?\n", initResult, err);
		return 2;
	}

	std::string why;
	const uint32_t handle = steam_ticket::Request(&why);
	Check("live: Steam gives a handle", handle != 0);
	std::string hex, waitWhy;
	bool done = false;
	std::thread worker([&] {
		hex = steam_ticket::Wait(handle, 10000, &waitWhy);
		done = true;
	});
	// The game's main thread runs Steam's callbacks every frame; so does this.
	for (int i = 0; i < 600 && !done; ++i)
	{
		run();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	worker.join();
	Check("live: the callback brought a ticket", !hex.empty());
	std::printf("ticket: %zu bytes%s\n", hex.size() / 2, hex.empty() ? (" (" + (handle ? waitWhy : why) + ")").c_str() : "");

	char key[256] = {};
	const DWORD keyLen = GetEnvironmentVariableA("STEAM_API_KEY", key, sizeof(key));
	if (!hex.empty() && keyLen > 0 && keyLen < sizeof(key))
	{
		const std::string path = std::string("/ISteamUserAuth/AuthenticateUserTicket/v1/?key=") + key + "&appid=4000&ticket=" + hex + "&identity=" + steam_ticket::kIdentity;
		DWORD status = 0;
		const std::string body = HttpsGet(std::wstring(path.begin(), path.end()), status);
		SecureZeroMemory(key, sizeof(key));
		const std::string me = std::to_string(self(user()));
		const bool ok = status == 200 && body.find("\"result\":\"OK\"") != std::string::npos && body.find("\"steamid\":\"" + me + "\"") != std::string::npos;
		Check("live: Steam's Web API says it's this account's ticket", ok);
		if (ok)
			std::printf("Web API: OK, the ticket is this account's: the backend's key can check Garry's Mod tickets\n");
		else
		{
			// Steam's answer, without anything that names the account.
			const size_t errAt = body.find("\"error\"");
			std::printf("Web API: HTTP %lu %s\n", static_cast<unsigned long>(status), errAt != std::string::npos ? body.substr(errAt, 120).c_str() : "(no error field)");
		}
	}
	else if (!hex.empty())
		std::printf("Web API: not asked (set STEAM_API_KEY to check the backend's key)\n");

	steam_ticket::Release(handle);
	steam_ticket::Pump();
	Check("live: cancelled on this thread", steam_ticket::detail::PendingCancels() == 0);
	shutdown();
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
	if (argc >= 3 && std::strcmp(argv[1], "--live") == 0)
		return Live(argv[2]);
	if (argc > 1)
	{
		std::printf("usage: steam_ticket_smoke [--live <GarrysMod folder>]\n");
		return 2;
	}
	return Offline();
}
