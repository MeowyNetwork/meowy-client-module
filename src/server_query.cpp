#include "server_query.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
	// --- Steam's types (steamworks_sdk matchmakingtypes.h / isteammatchmaking.h) ---
	struct servernetadr_t
	{
		uint16_t connectionPort;
		uint16_t queryPort;
		uint32_t ip;
	};

	struct gameserveritem_t
	{
		servernetadr_t netAdr;
		int ping;
		bool hadSuccessfulResponse;
		bool doNotRefresh;
		char gameDir[32];
		char map[32];
		char gameDescription[64];
		uint32_t appId;
		int players;
		int maxPlayers;
		int botPlayers;
		bool password;
		bool secure;
		uint32_t timeLastPlayed;
		int serverVersion;
		char serverName[64];
		char gameTags[128];
		uint64_t steamId;
	};
	static_assert(offsetof(gameserveritem_t, appId) == 144, "gameserveritem_t layout");
	static_assert(offsetof(gameserveritem_t, serverName) == 172, "gameserveritem_t layout");
	static_assert(offsetof(gameserveritem_t, steamId) == 368, "gameserveritem_t layout");

	struct MatchMakingKeyValuePair_t
	{
		char key[256];
		char value[256];
	};

	using HServerListRequest = void*;

	// ISteamMatchmakingServerListResponse: Steam calls these (thiscall on
	// 32-bit, as MSVC's virtual calls are) from SteamAPI_RunCallbacks.
	class ListResponse
	{
	public:
		virtual void ServerResponded(HServerListRequest request, int index);
		virtual void ServerFailedToRespond(HServerListRequest request, int index);
		virtual void RefreshComplete(HServerListRequest request, int response);
		int id = 0;
	};

#ifdef _WIN32
	using GetServersFn = void* (__cdecl*)();
	using RequestListFn = HServerListRequest (__cdecl*)(void* self, uint32_t app, MatchMakingKeyValuePair_t** filters,
		uint32_t count, ListResponse* response);
	using RequestFriendsFn = HServerListRequest (__cdecl*)(void* self, uint32_t app, MatchMakingKeyValuePair_t** filters,
		uint32_t count, ListResponse* response);
	using DetailsFn = gameserveritem_t* (__cdecl*)(void* self, HServerListRequest request, int index);
	using RequestOpFn = void (__cdecl*)(void* self, HServerListRequest request);

#ifdef _WIN64
	constexpr const wchar_t* kSteamApi = L"steam_api64.dll";
#else
	constexpr const wchar_t* kSteamApi = L"steam_api.dll";
#endif

	GetServersFn g_get = nullptr;
	RequestListFn g_internet = nullptr;
	RequestFriendsFn g_friends = nullptr;
	DetailsFn g_details = nullptr;
	RequestOpFn g_cancel = nullptr;
	RequestOpFn g_release = nullptr;
	bool g_resolved = false, g_broken = false;

	bool Resolve()
	{
		if (g_broken)
			return false;
		if (g_resolved)
			return true;
		HMODULE steam = GetModuleHandleW(kSteamApi);
		if (!steam)
			return false;
		g_get = reinterpret_cast<GetServersFn>(GetProcAddress(steam, "SteamAPI_SteamMatchmakingServers_v002"));
		g_internet = reinterpret_cast<RequestListFn>(GetProcAddress(steam, "SteamAPI_ISteamMatchmakingServers_RequestInternetServerList"));
		g_friends = reinterpret_cast<RequestFriendsFn>(GetProcAddress(steam, "SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList"));
		g_details = reinterpret_cast<DetailsFn>(GetProcAddress(steam, "SteamAPI_ISteamMatchmakingServers_GetServerDetails"));
		g_cancel = reinterpret_cast<RequestOpFn>(GetProcAddress(steam, "SteamAPI_ISteamMatchmakingServers_CancelQuery"));
		g_release = reinterpret_cast<RequestOpFn>(GetProcAddress(steam, "SteamAPI_ISteamMatchmakingServers_ReleaseRequest"));
		if (!g_get || !g_internet || !g_friends || !g_details || !g_cancel || !g_release)
		{
			g_broken = true;
			return false;
		}
		g_resolved = true;
		return true;
	}

	int OnFault(unsigned long code)
	{
		return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
	}

	// No C++ objects in these: they use structured exception handling.
	HServerListRequest SafeRequest(bool friends, MatchMakingKeyValuePair_t** filters, uint32_t count, ListResponse* response)
	{
		__try
		{
			void* servers = g_get();
			if (!servers)
				return nullptr;
			return friends ? g_friends(servers, 4000, filters, count, response) : g_internet(servers, 4000, filters, count, response);
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
			return nullptr;
		}
	}

	bool SafeDetails(HServerListRequest request, int index, gameserveritem_t* out)
	{
		__try
		{
			void* servers = g_get();
			gameserveritem_t* item = servers ? g_details(servers, request, index) : nullptr;
			if (!item)
				return false;
			std::memcpy(out, item, sizeof(*out));
			return true;
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
			return false;
		}
	}

	void SafeStop(HServerListRequest request, bool release)
	{
		__try
		{
			void* servers = g_get();
			if (!servers)
				return;
			g_cancel(servers, request);
			if (release)
				g_release(servers, request);
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
		}
	}
#endif

	struct Request
	{
		HServerListRequest handle = nullptr;
		std::unique_ptr<ListResponse> response;
		std::vector<server_query::Server> pending;
		bool finished = false;
		int failed = 0, total = 0;
		double started = 0, finishedAt = 0;
	};

	std::map<int, Request> g_requests;   // main thread only
	int g_nextId = 0;
	std::string g_last;

	double Now()
	{
#ifdef _WIN32
		return GetTickCount64() / 1000.0;
#else
		return 0;
#endif
	}

	std::string Text(const char* s, size_t size)
	{
		size_t n = 0;
		while (n < size && s[n])
			++n;
		return std::string(s, n);
	}

	Request* Find(int id)
	{
		auto it = g_requests.find(id);
		return it == g_requests.end() ? nullptr : &it->second;
	}

	void ListResponse::ServerResponded(HServerListRequest request, int index)
	{
#ifdef _WIN32
		Request* r = Find(id);
		if (!r || (r->handle && r->handle != request))
			return;
		gameserveritem_t item;
		if (!SafeDetails(request, index, &item) || !item.hadSuccessfulResponse)
		{
			++r->failed;
			return;
		}
		server_query::Server s;
		char address[32];
		std::snprintf(address, sizeof(address), "%u.%u.%u.%u:%u", (item.netAdr.ip >> 24) & 255, (item.netAdr.ip >> 16) & 255,
			(item.netAdr.ip >> 8) & 255, item.netAdr.ip & 255, item.netAdr.connectionPort);
		s.address = address;
		s.name = Text(item.serverName, sizeof(item.serverName));
		s.map = Text(item.map, sizeof(item.map));
		s.gameDir = Text(item.gameDir, sizeof(item.gameDir));
		s.description = Text(item.gameDescription, sizeof(item.gameDescription));
		s.tags = Text(item.gameTags, sizeof(item.gameTags));
		s.ping = item.ping;
		s.players = item.players;
		s.maxPlayers = item.maxPlayers;
		s.bots = item.botPlayers;
		s.password = item.password;
		s.secure = item.secure;
		s.lastPlayed = item.timeLastPlayed;
		s.steamId = item.steamId;
		s.anonymous = ((item.steamId >> 52) & 0xF) == 4;   // k_EAccountTypeAnonGameServer
		++r->total;
		if (r->pending.size() < 20000)
			r->pending.push_back(std::move(s));
#else
		(void)request; (void)index;
#endif
	}

	void ListResponse::ServerFailedToRespond(HServerListRequest request, int)
	{
		Request* r = Find(id);
		if (r && (!r->handle || r->handle == request))
			++r->failed;
	}

	void ListResponse::RefreshComplete(HServerListRequest request, int)
	{
		Request* r = Find(id);
		if (r && (!r->handle || r->handle == request) && !r->finished)
		{
			r->finished = true;
			r->finishedAt = Now();
		}
	}
}

namespace server_query
{
	namespace detail
	{
		bool FilterAllowed(const std::string& key, const std::string& value)
		{
			static const char* const kKeys[] = { "gamedir", "map", "gametagsand", "gametagsnor", "notfull", "hasplayers",
				"noplayers", "secure", "dedicated", "name_match", "white", "proxy", "empty", "full", "nand", "nor", "and", "or" };
			bool known = false;
			for (const char* k : kKeys)
				known = known || key == k;
			if (!known || value.empty() || value.size() > 127)
				return false;
			for (unsigned char c : value)
				if (c < 32 || c == 127 || c == '\\')
					return false;
			return true;
		}
	}

	int Start(List list, const Filters& filters, std::string& why)
	{
#ifdef _WIN32
		if (filters.size() > 16)
		{
			why = "too many filters";
			return 0;
		}
		for (const auto& f : filters)
		{
			if (!detail::FilterAllowed(f.first, f.second))
			{
				why = "filter not allowed: " + f.first;
				return 0;
			}
		}
		if (!Resolve())
		{
			why = "Steam's server lists can't be reached";
			return 0;
		}
		std::vector<MatchMakingKeyValuePair_t> pairs(filters.size());
		std::vector<MatchMakingKeyValuePair_t*> pointers(filters.size());
		for (size_t i = 0; i < filters.size(); ++i)
		{
			std::snprintf(pairs[i].key, sizeof(pairs[i].key), "%s", filters[i].first.c_str());
			std::snprintf(pairs[i].value, sizeof(pairs[i].value), "%s", filters[i].second.c_str());
			pointers[i] = &pairs[i];
		}
		const int id = ++g_nextId;
		Request& r = g_requests[id];
		r.response.reset(new ListResponse());
		r.response->id = id;
		r.started = Now();
		r.handle = SafeRequest(list == List::Friends, pointers.empty() ? nullptr : pointers.data(),
			static_cast<uint32_t>(pointers.size()), r.response.get());
		if (!r.handle)
		{
			g_requests.erase(id);
			why = "Steam didn't start the list";
			return 0;
		}
		// At most a few lists at once: the oldest are stopped.
		while (g_requests.size() > 4)
			Cancel(g_requests.begin()->first);
		return id;
#else
		(void)list; (void)filters;
		why = "Windows only";
		return 0;
#endif
	}

	std::vector<Server> Take(int id, bool* finished, int* failed)
	{
		std::vector<Server> out;
		Request* r = Find(id);
		if (finished)
			*finished = !r || r->finished;
		if (failed)
			*failed = r ? r->failed : 0;
		if (!r)
			return out;
		out.swap(r->pending);
		if (r->finished)
		{
			char line[160];
			std::snprintf(line, sizeof(line), "list %d: %d servers answered, %d didn't, %.1f s", id, r->total, r->failed,
				r->finishedAt - r->started);
			g_last = line;
		}
		return out;
	}

	void Cancel(int id)
	{
		auto it = g_requests.find(id);
		if (it == g_requests.end())
			return;
#ifdef _WIN32
		if (it->second.handle && g_resolved && !g_broken)
			SafeStop(it->second.handle, true);
#endif
		g_requests.erase(it);
	}

	std::string Describe()
	{
		std::string out = std::to_string(g_requests.size()) + " lists open";
		for (const auto& kv : g_requests)
		{
			char line[160];
			std::snprintf(line, sizeof(line), "; list %d: %d answered, %d didn't%s", kv.first, kv.second.total, kv.second.failed,
				kv.second.finished ? ", complete" : "");
			out += line;
		}
		if (!g_last.empty())
			out += "; last complete " + g_last;
		return out;
	}
}
