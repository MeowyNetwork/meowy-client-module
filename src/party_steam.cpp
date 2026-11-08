#include "party_steam.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace party_steam
{
	namespace detail
	{
		uint64_t ParseLobbyId(const std::string& text)
		{
			if (text.empty() || text.size() > 20)
				return 0;
			for (char c : text)
				if (c < '0' || c > '9')
					return 0;
			const uint64_t id = std::strtoull(text.c_str(), nullptr, 10);
			// Universe 1 (public), account type 8 (chat): what every lobby's SteamID is.
			return (id >> 52) == ((1ull << 4) | 8ull) ? id : 0;
		}

		uint64_t LobbyFromCommandLine(const std::wstring& line)
		{
			const std::wstring key = L"+connect_lobby";
			const size_t at = line.find(key);
			if (at == std::wstring::npos)
				return 0;
			size_t i = at + key.size();
			while (i < line.size() && (line[i] == L' ' || line[i] == L'\t' || line[i] == L'"'))
				++i;
			std::string digits;
			while (i < line.size() && line[i] >= L'0' && line[i] <= L'9' && digits.size() < 21)
				digits += static_cast<char>(line[i++]);
			return ParseLobbyId(digits);
		}

		std::string Refusal(const char* version, uint64_t owner, uint64_t self, int ownerRelationship)
		{
			if (!version || std::strcmp(version, "1") != 0)
				return "that isn't a Meowy party";
			if (!owner)
				return "Steam couldn't say who leads that party";
			if (owner != self && ownerRelationship != kFriend)
				return "that party's leader isn't on your Steam friends list";
			return std::string();
		}
	}
}

#ifdef _WIN32
namespace
{
	// --- Steam's flat API (steam_api_flat.h), __cdecl on 32-bit ---------------------------
	using AccessorFn = void* (__cdecl*)();
	using CreateLobbyFn = uint64_t (__cdecl*)(void* mm, int type, int maxMembers);
	using JoinLobbyFn = uint64_t (__cdecl*)(void* mm, uint64_t lobby);
	using LeaveLobbyFn = void (__cdecl*)(void* mm, uint64_t lobby);
	using InviteToLobbyFn = bool (__cdecl*)(void* mm, uint64_t lobby, uint64_t invitee);
	using NumMembersFn = int (__cdecl*)(void* mm, uint64_t lobby);
	using MemberByIndexFn = uint64_t (__cdecl*)(void* mm, uint64_t lobby, int index);
	using GetLobbyDataFn = const char* (__cdecl*)(void* mm, uint64_t lobby, const char* key);
	using SetLobbyDataFn = bool (__cdecl*)(void* mm, uint64_t lobby, const char* key, const char* value);
	using GetMemberDataFn = const char* (__cdecl*)(void* mm, uint64_t lobby, uint64_t user, const char* key);
	using SetMemberDataFn = void (__cdecl*)(void* mm, uint64_t lobby, const char* key, const char* value);
	using LobbyOwnerFn = uint64_t (__cdecl*)(void* mm, uint64_t lobby);
	using SetPresenceFn = bool (__cdecl*)(void* fr, const char* key, const char* value);
	using FriendNameFn = const char* (__cdecl*)(void* fr, uint64_t user);
	using InviteDialogFn = void (__cdecl*)(void* fr, uint64_t lobby);
	using FriendCountFn = int (__cdecl*)(void* fr, int flags);
	using FriendByIndexFn = uint64_t (__cdecl*)(void* fr, int index, int flags);
	using FriendGameFn = bool (__cdecl*)(void* fr, uint64_t user, void* info);
	using FriendRelationshipFn = int (__cdecl*)(void* fr, uint64_t user);
	using InviteToGameFn = bool (__cdecl*)(void* fr, uint64_t user, const char* connect);
	using FriendPresenceFn = const char* (__cdecl*)(void* fr, uint64_t user, const char* key);
	using RequestPresenceFn = void (__cdecl*)(void* fr, uint64_t user);
	using CallDoneFn = bool (__cdecl*)(void* ut, uint64_t call, bool* failed);
	using CallResultFn = bool (__cdecl*)(void* ut, uint64_t call, void* out, int size, int expected, bool* failed);
	using SelfIdFn = uint64_t (__cdecl*)(void* user);

	class CCallbackBase;
	using RegisterCallbackFn = void (__cdecl*)(CCallbackBase* cb, int id);

#ifdef _WIN64
	constexpr const wchar_t* kSteamApi = L"steam_api64.dll";
#else
	constexpr const wchar_t* kSteamApi = L"steam_api.dll";
#endif

	struct Api
	{
		AccessorFn matchmaking, friends, utils, user;
		CreateLobbyFn createLobby;
		JoinLobbyFn joinLobby;
		LeaveLobbyFn leaveLobby;
		InviteToLobbyFn inviteToLobby;
		NumMembersFn numMembers;
		MemberByIndexFn memberByIndex;
		GetLobbyDataFn getLobbyData;
		SetLobbyDataFn setLobbyData;
		GetMemberDataFn getMemberData;
		SetMemberDataFn setMemberData;
		LobbyOwnerFn lobbyOwner;
		SetPresenceFn setPresence;
		FriendNameFn friendName;
		InviteDialogFn inviteDialog;
		FriendCountFn friendCount;
		FriendByIndexFn friendByIndex;
		FriendGameFn friendGame;
		FriendRelationshipFn friendRelationship;
		InviteToGameFn inviteToGame;
		FriendPresenceFn friendPresence;
		RequestPresenceFn requestPresence;
		CallDoneFn callDone;
		CallResultFn callResult;
		SelfIdFn selfId;
		RegisterCallbackFn registerCallback;
	};
	Api g_api{};
	bool g_resolved = false;
	bool g_broken = false;          // Steam's code faulted, or the exports are missing: off for the session
	std::string g_why = "not looked up yet";

	template <typename T>
	bool Find(HMODULE dll, T& out, const char* name)
	{
		out = reinterpret_cast<T>(GetProcAddress(dll, name));
		return out != nullptr;
	}

	bool Resolve()
	{
		if (g_broken)
			return false;
		if (g_resolved)
			return true;
		HMODULE dll = GetModuleHandleW(kSteamApi);
		if (!dll)
		{
			g_why = "Steam's API isn't loaded";
			return false;
		}
		Api& a = g_api;
		const bool ok = Find(dll, a.matchmaking, "SteamAPI_SteamMatchmaking_v009")
			&& Find(dll, a.friends, "SteamAPI_SteamFriends_v018")
			&& Find(dll, a.utils, "SteamAPI_SteamUtils_v010")
			&& Find(dll, a.user, "SteamAPI_SteamUser_v023")
			&& Find(dll, a.createLobby, "SteamAPI_ISteamMatchmaking_CreateLobby")
			&& Find(dll, a.joinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby")
			&& Find(dll, a.leaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby")
			&& Find(dll, a.inviteToLobby, "SteamAPI_ISteamMatchmaking_InviteUserToLobby")
			&& Find(dll, a.numMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers")
			&& Find(dll, a.memberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex")
			&& Find(dll, a.getLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData")
			&& Find(dll, a.setLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData")
			&& Find(dll, a.getMemberData, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData")
			&& Find(dll, a.setMemberData, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData")
			&& Find(dll, a.lobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner")
			&& Find(dll, a.setPresence, "SteamAPI_ISteamFriends_SetRichPresence")
			&& Find(dll, a.friendName, "SteamAPI_ISteamFriends_GetFriendPersonaName")
			&& Find(dll, a.inviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog")
			&& Find(dll, a.friendCount, "SteamAPI_ISteamFriends_GetFriendCount")
			&& Find(dll, a.friendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex")
			&& Find(dll, a.friendGame, "SteamAPI_ISteamFriends_GetFriendGamePlayed")
			&& Find(dll, a.friendRelationship, "SteamAPI_ISteamFriends_GetFriendRelationship")
			&& Find(dll, a.inviteToGame, "SteamAPI_ISteamFriends_InviteUserToGame")
			&& Find(dll, a.friendPresence, "SteamAPI_ISteamFriends_GetFriendRichPresence")
			&& Find(dll, a.requestPresence, "SteamAPI_ISteamFriends_RequestFriendRichPresence")
			&& Find(dll, a.callDone, "SteamAPI_ISteamUtils_IsAPICallCompleted")
			&& Find(dll, a.callResult, "SteamAPI_ISteamUtils_GetAPICallResult")
			&& Find(dll, a.selfId, "SteamAPI_ISteamUser_GetSteamID")
			&& Find(dll, a.registerCallback, "SteamAPI_RegisterCallback");
		if (!ok)
		{
			g_broken = true;
			g_why = "this steam_api lacks the lobby or friends calls";
			return false;
		}
		g_resolved = true;
		g_why.clear();
		return true;
	}

	int OnFault(unsigned long code)
	{
		return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
	}

	// Runs `f` (plain calls into Steam, results into its captures) with a fault
	// turning parties off for the session instead of taking the game down.
	template <typename F>
	bool Guard(F& f)
	{
		__try
		{
			f();
			return true;
		}
		__except (OnFault(GetExceptionCode()))
		{
			g_broken = true;
			return false;
		}
	}

	struct Ifaces
	{
		void* mm = nullptr;
		void* fr = nullptr;
		void* ut = nullptr;
		void* us = nullptr;
	};

	bool Get(Ifaces& i)
	{
		if (!Resolve())
			return false;
		auto f = [&] {
			i.mm = g_api.matchmaking();
			i.fr = g_api.friends();
			i.ut = g_api.utils();
			i.us = g_api.user();
		};
		if (!Guard(f))
		{
			g_why = "Steam faulted";
			return false;
		}
		if (!i.mm || !i.fr || !i.ut || !i.us)
		{
			g_why = "the Steam client doesn't offer these interfaces";
			return false;
		}
		return true;
	}

	std::string Copy(const char* s, size_t max)
	{
		if (!s)
			return "";
		std::string out(s, strnlen(s, max));
		return out;
	}

	// --- Call results and the one callback (isteammatchmaking.h, isteamfriends.h) ---------
	// Windows builds of steam_api pack callback structs to 8 bytes.
#pragma pack(push, 8)
	struct LobbyCreated { int result; uint64_t lobby; };                                    // 513
	struct LobbyEnter { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };   // 504
	struct LobbyJoinRequested { uint64_t lobby; uint64_t friendId; };                        // 333
	struct FriendGameInfo { uint64_t gameId; uint32_t ip; uint16_t port, queryPort; uint64_t lobby; };
#pragma pack(pop)
	constexpr int kLobbyCreated = 513, kLobbyEnter = 504, kJoinRequested = 333;
	constexpr int kLobbyFriendsOnly = 1;
	constexpr int kFriendFlagImmediate = 0x04;
	static_assert(sizeof(LobbyCreated) == 16 && sizeof(LobbyEnter) == 24 && sizeof(LobbyJoinRequested) == 16, "Steam's layouts");

	// Steam's CCallbackBase (steam_api_common.h), declared the same way so this
	// compiler lays out the vtable and fields as Steam's own build expects.
	class CCallbackBase
	{
	public:
		CCallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
		virtual void Run(void* pvParam) = 0;
		virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
		int GetICallback() { return m_iCallback; }
		virtual int GetCallbackSizeBytes() = 0;

	protected:
		uint8_t m_nCallbackFlags;
		int m_iCallback;
	};

	std::mutex g_mutex;
	std::vector<std::string> g_events;
	void Event(const std::string& e)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_events.size() < 32)
			g_events.push_back(e);
	}

	class JoinRequested final : public CCallbackBase
	{
	public:
		void Run(void* p) override
		{
			const LobbyJoinRequested* r = static_cast<const LobbyJoinRequested*>(p);
			if (!r || !r->lobby)
				return;
			// Who it came from: a friend's invite, or a join link naming anyone.
			// Not a friend: the party the player is in isn't left for it.
			int relationship = party_steam::detail::kFriend;
			Ifaces i;
			if (r->friendId && Get(i))
			{
				const uint64_t from = r->friendId;
				auto f = [&] { relationship = g_api.friendRelationship(i.fr, from); };
				if (!Guard(f))
					relationship = 0;
			}
			if (relationship != party_steam::detail::kFriend)
				Event("failed: that invite isn't from one of your Steam friends");
			else
				Event("invite " + std::to_string(r->lobby));
		}
		void Run(void* p, bool, uint64_t) override { Run(p); }
		int GetCallbackSizeBytes() override { return sizeof(LobbyJoinRequested); }
	};
	JoinRequested g_joinRequested;
	bool g_started = false;

	uint64_t g_lobby = 0;
	uint64_t g_call = 0;
	int g_callKind = 0;   // 1 create, 2 join
	uint64_t g_joining = 0;
	std::string g_last = "none yet";

}

namespace party_steam
{
	std::string Unavailable()
	{
		Ifaces i;
		return Get(i) ? "" : g_why;
	}

	void Start()
	{
		if (g_started)
			return;
		Ifaces i;
		if (!Get(i))
			return;
		g_started = true;
		auto f = [&] { g_api.registerCallback(&g_joinRequested, kJoinRequested); };
		Guard(f);
		const uint64_t lobby = detail::LobbyFromCommandLine(GetCommandLineW());
		if (lobby)
			Event("invite " + std::to_string(lobby));
	}

	bool Create(int maxMembers)
	{
		Ifaces i;
		if (g_call || !Get(i))
			return false;
		uint64_t call = 0;
		auto f = [&] { call = g_api.createLobby(i.mm, kLobbyFriendsOnly, maxMembers); };
		if (!Guard(f) || !call)
			return false;
		g_call = call;
		g_callKind = 1;
		g_last = "creating";
		return true;
	}

	bool Join(uint64_t lobby)
	{
		Ifaces i;
		if (g_call || !lobby || !Get(i))
			return false;
		uint64_t call = 0;
		auto f = [&] { call = g_api.joinLobby(i.mm, lobby); };
		if (!Guard(f) || !call)
			return false;
		g_call = call;
		g_callKind = 2;
		g_joining = lobby;
		g_last = "joining " + std::to_string(lobby);
		return true;
	}

	bool SetPresence(const char* key, const std::string& value)
	{
		Ifaces i;
		if (!Get(i))
			return false;
		bool ok = false;
		const std::string v = value.substr(0, 255);
		auto f = [&] { ok = g_api.setPresence(i.fr, key, v.c_str()); };
		return Guard(f) && ok;
	}

	void Leave()
	{
		Ifaces i;
		if (!Get(i))
			return;
		if (g_lobby)
		{
			const uint64_t lobby = g_lobby;
			auto f = [&] { g_api.leaveLobby(i.mm, lobby); };
			Guard(f);
		}
		g_lobby = 0;
		g_last = "left";
		SetPresence("steam_player_group", "");
		SetPresence("steam_player_group_size", "");
	}

	bool OpenInviteDialog()
	{
		Ifaces i;
		if (!g_lobby || !Get(i))
			return false;
		const uint64_t lobby = g_lobby;
		auto f = [&] { g_api.inviteDialog(i.fr, lobby); };
		return Guard(f);
	}

	bool Invite(uint64_t friendId)
	{
		Ifaces i;
		if (!g_lobby || !friendId || !Get(i))
			return false;
		bool ok = false;
		const uint64_t lobby = g_lobby;
		auto f = [&] { ok = g_api.inviteToLobby(i.mm, lobby, friendId); };
		return Guard(f) && ok;
	}

	bool InviteToServer(uint64_t friendId, const std::string& address)
	{
		Ifaces i;
		if (!friendId || address.empty() || address.size() > 21 || !Get(i))
			return false;
		for (char c : address)
			if (!((c >= '0' && c <= '9') || c == '.' || c == ':'))
				return false;
		const std::string connect = "+connect " + address;
		bool ok = false;
		auto f = [&] { ok = g_api.inviteToGame(i.fr, friendId, connect.c_str()); };
		return Guard(f) && ok;
	}

	bool SetLobbyData(const char* key, const std::string& value)
	{
		Ifaces i;
		if (!g_lobby || !Get(i))
			return false;
		bool ok = false;
		const uint64_t lobby = g_lobby;
		const std::string v = value.substr(0, 255);
		auto f = [&] { ok = g_api.setLobbyData(i.mm, lobby, key, v.c_str()); };
		return Guard(f) && ok;
	}

	bool SetMemberData(const char* key, const std::string& value)
	{
		Ifaces i;
		if (!g_lobby || !Get(i))
			return false;
		const uint64_t lobby = g_lobby;
		const std::string v = value.substr(0, 255);
		auto f = [&] { g_api.setMemberData(i.mm, lobby, key, v.c_str()); };
		return Guard(f);
	}

	State Read()
	{
		State s;
		Ifaces i;
		if (!g_lobby || !Get(i))
			return s;
		const uint64_t lobby = g_lobby;
		int count = 0;
		uint64_t owner = 0, self = 0;
		const char *srv = nullptr, *srvName = nullptr, *go = nullptr;
		auto f = [&] {
			count = g_api.numMembers(i.mm, lobby);
			owner = g_api.lobbyOwner(i.mm, lobby);
			self = g_api.selfId(i.us);
			srv = g_api.getLobbyData(i.mm, lobby, "srv");
			srvName = g_api.getLobbyData(i.mm, lobby, "srvname");
			go = g_api.getLobbyData(i.mm, lobby, "go");
		};
		if (!Guard(f))
			return s;
		s.lobby = lobby;
		s.owner = owner;
		s.self = self;
		s.srv = Copy(srv, 64);
		s.srvName = Copy(srvName, 128);
		s.go = Copy(go, 64);
		for (int m = 0; m < count && m < 16; ++m)
		{
			uint64_t id = 0;
			const char *name = nullptr, *at = nullptr, *atName = nullptr, *pack = nullptr;
			auto g = [&] {
				id = g_api.memberByIndex(i.mm, lobby, m);
				name = g_api.friendName(i.fr, id);
				at = g_api.getMemberData(i.mm, lobby, id, "at");
				atName = g_api.getMemberData(i.mm, lobby, id, "atname");
				pack = g_api.getMemberData(i.mm, lobby, id, "pack");
			};
			if (!Guard(g))
				break;
			Member member;
			member.id = id;
			member.name = Copy(name, 64);
			member.at = Copy(at, 64);
			member.atName = Copy(atName, 128);
			member.pack = Copy(pack, 32);
			s.members.push_back(std::move(member));
		}
		return s;
	}

	std::vector<std::string> Poll()
	{
		Ifaces i;
		if (g_call && Get(i))
		{
			bool failed = false, done = false;
			const uint64_t call = g_call;
			auto f = [&] { done = g_api.callDone(i.ut, call, &failed); };
			if (!Guard(f))
			{
				g_call = 0;
				Event("failed: Steam faulted");
			}
			else if (done)
			{
				g_call = 0;
				bool got = false;
				if (g_callKind == 1)
				{
					LobbyCreated r{};
					auto g = [&] { got = g_api.callResult(i.ut, call, &r, sizeof(r), kLobbyCreated, &failed); };
					if (Guard(g) && got && !failed && r.result == 1 && r.lobby)
					{
						g_lobby = r.lobby;
						g_last = "created " + std::to_string(r.lobby);
						SetLobbyData("v", "1");
						Event("created");
					}
					else
					{
						g_last = "create failed (result " + std::to_string(r.result) + ")";
						Event("failed: Steam couldn't create the party");
					}
				}
				else
				{
					LobbyEnter r{};
					auto g = [&] { got = g_api.callResult(i.ut, call, &r, sizeof(r), kLobbyEnter, &failed); };
					if (Guard(g) && got && !failed && r.response == 1 && r.lobby == g_joining)
					{
						g_lobby = r.lobby;
						// Only a Meowy party led by a friend (or this player): anything
						// else's lobby is left at once, before the pack publishes anything in it.
						const char* v = nullptr;
						uint64_t owner = 0, self = 0;
						int relationship = 0;
						const uint64_t lobby = g_lobby;
						auto h = [&] {
							v = g_api.getLobbyData(i.mm, lobby, "v");
							owner = g_api.lobbyOwner(i.mm, lobby);
							self = g_api.selfId(i.us);
							relationship = owner ? g_api.friendRelationship(i.fr, owner) : 0;
						};
						const std::string why = Guard(h) ? detail::Refusal(v, owner, self, relationship) : "Steam faulted";
						if (why.empty())
						{
							g_last = "joined " + std::to_string(r.lobby);
							Event("joined");
						}
						else
						{
							Leave();
							g_last = "left " + std::to_string(r.lobby) + ": " + why;
							Event("failed: " + why);
						}
					}
					else
					{
						g_last = "join failed (response " + std::to_string(r.response) + ")";
						Event(r.response == 4 ? "failed: the party is full" : "failed: Steam couldn't join the party");
					}
				}
			}
		}
		std::lock_guard<std::mutex> lock(g_mutex);
		std::vector<std::string> out;
		out.swap(g_events);
		return out;
	}

	std::vector<Friend> Friends(size_t max)
	{
		std::vector<Friend> playing, others;
		Ifaces i;
		if (!Get(i))
			return playing;
		int count = 0;
		auto f = [&] { count = g_api.friendCount(i.fr, kFriendFlagImmediate); };
		if (!Guard(f))
			return playing;
		for (int n = 0; n < count && n < 2000; ++n)
		{
			uint64_t id = 0;
			const char *name = nullptr, *pack = nullptr;
			FriendGameInfo game{};
			bool inGame = false;
			auto g = [&] {
				id = g_api.friendByIndex(i.fr, n, kFriendFlagImmediate);
				name = g_api.friendName(i.fr, id);
				inGame = g_api.friendGame(i.fr, id, &game);
				if (inGame && (game.gameId & 0xFFFFFF) == 4000)
				{
					g_api.requestPresence(i.fr, id);
					pack = g_api.friendPresence(i.fr, id, "meowy_pack");
				}
			};
			if (!Guard(g))
				break;
			Friend fr;
			fr.id = id;
			fr.name = Copy(name, 64);
			fr.inGmod = inGame && (game.gameId & 0xFFFFFF) == 4000;
			fr.hasPack = pack && *pack;
			(fr.inGmod ? playing : others).push_back(std::move(fr));
		}
		for (Friend& f2 : others)
			playing.push_back(std::move(f2));
		if (playing.size() > max)
			playing.resize(max);
		return playing;
	}

	std::string Describe()
	{
		const std::string why = Unavailable();
		if (!why.empty())
			return "parties: unavailable (" + why + ")";
		return std::string("parties: ") + (g_lobby ? "in party " + std::to_string(g_lobby) : "no party")
			+ (g_call ? ", waiting for Steam" : "") + "; last: " + g_last + (g_started ? "" : "; invite callback not registered");
	}
}
#else
namespace party_steam
{
	std::string Unavailable() { return "Windows only"; }
	void Start() {}
	bool Create(int) { return false; }
	bool Join(uint64_t) { return false; }
	void Leave() {}
	bool OpenInviteDialog() { return false; }
	bool Invite(uint64_t) { return false; }
	bool InviteToServer(uint64_t, const std::string&) { return false; }
	bool SetLobbyData(const char*, const std::string&) { return false; }
	bool SetMemberData(const char*, const std::string&) { return false; }
	bool SetPresence(const char*, const std::string&) { return false; }
	State Read() { return State(); }
	std::vector<std::string> Poll() { return {}; }
	std::vector<Friend> Friends(size_t) { return {}; }
	std::string Describe() { return "parties: Windows only"; }
}
#endif
