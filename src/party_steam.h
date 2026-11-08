#pragma once
// Parties (roadmap MR3, docs/server-index-and-parties.md): friends who move
// between servers together, through a friends-only Steam lobby.
//
// Like timeline_steam.h, nothing of Steam's is shipped or started here: Garry's
// Mod has already loaded steam_api(64).dll and called SteamAPI_Init. Its flat
// API exports are looked up (GetModuleHandle, never loaded by us):
//   SteamAPI_SteamMatchmaking_v009, SteamAPI_SteamFriends_v018,
//   SteamAPI_SteamUtils_v010, SteamAPI_SteamUser_v023,
//   SteamAPI_ISteamMatchmaking_* (CreateLobby, JoinLobby, LeaveLobby,
//     InviteUserToLobby, Get/SetLobbyData, Get/SetLobbyMemberData,
//     GetNumLobbyMembers, GetLobbyMemberByIndex, GetLobbyOwner),
//   SteamAPI_ISteamFriends_* (SetRichPresence, GetPersonaName,
//     GetFriendPersonaName, ActivateGameOverlayInviteDialog, GetFriendCount,
//     GetFriendByIndex, GetFriendGamePlayed, GetFriendRelationship, InviteUserToGame,
//     GetFriendRichPresence, RequestFriendRichPresence),
//   SteamAPI_ISteamUtils_IsAPICallCompleted / GetAPICallResult,
//   SteamAPI_ISteamUser_GetSteamID, SteamAPI_RegisterCallback.
// Garry's Mod's bundled steam_api exports all of them on both architectures,
// and the game itself uses no lobbies, so their invites are the pack's.
//
// Everything runs on the game's main thread: menu-state Lua calls in, and the
// one callback (GameLobbyJoinRequested_t: a friend's invite accepted in Steam)
// arrives from SteamAPI_RunCallbacks, which the game runs each frame. A fault
// inside Steam's code turns parties off for the session.

#include <cstdint>
#include <string>
#include <vector>

namespace party_steam
{
	// Whether Steam's lobby and friends interfaces can be reached ("" if so, else why not).
	std::string Unavailable();

	// Registers the invite callback and reads "+connect_lobby <id>" from the
	// command line (Steam starts the game that way from an invite). Once.
	void Start();

	// A new friends-only lobby for up to `maxMembers`. False if one can't be asked for.
	bool Create(int maxMembers);
	// Joins a lobby by its SteamID. False if one can't be asked for.
	bool Join(uint64_t lobby);
	// Leaves the party (and clears the pack's Rich Presence keys).
	void Leave();

	// Steam's own invite dialog for the party.
	bool OpenInviteDialog();
	// Invites one friend to the party (a Steam chat invite).
	bool Invite(uint64_t friendId);
	// Invites a friend straight to a server, the stock way ("+connect a.b.c.d:port"):
	// for friends without the pack, whose game handles it itself.
	bool InviteToServer(uint64_t friendId, const std::string& address);

	// Lobby data (owner only) and this member's data. Keys are fixed by the caller.
	bool SetLobbyData(const char* key, const std::string& value);
	bool SetMemberData(const char* key, const std::string& value);
	// The pack's Rich Presence keys (steam_player_group, steam_player_group_size, meowy_pack).
	bool SetPresence(const char* key, const std::string& value);

	struct Member
	{
		uint64_t id = 0;
		std::string name, at, atName, pack;
	};
	struct State
	{
		uint64_t lobby = 0, owner = 0, self = 0;
		std::vector<Member> members;
		std::string srv, srvName, go;   // the owner's lobby data
	};
	// The party as Steam's local copy has it (lobby 0: none).
	State Read();

	// What happened since the last call: "created", "joined", "failed: ...",
	// "invite <lobby id>" (accepted in Steam or from the command line).
	std::vector<std::string> Poll();

	struct Friend
	{
		uint64_t id = 0;
		std::string name;
		bool inGmod = false;    // playing Garry's Mod right now
		bool hasPack = false;   // their Rich Presence has meowy_pack
	};
	// Steam friends, those playing Garry's Mod first, at most `max`.
	std::vector<Friend> Friends(size_t max);

	// One line for a status command.
	std::string Describe();

	namespace detail
	{
		// "+connect_lobby 109775241234567890" in a command line, else 0.
		uint64_t LobbyFromCommandLine(const std::wstring& line);
		// A SteamID64 of a lobby (chat-room type, 0x0186...), else 0.
		uint64_t ParseLobbyId(const std::string& text);
		// EFriendRelationship's "friend".
		constexpr int kFriend = 3;
		// Why a lobby just entered isn't a party to stay in ("" if it is): only
		// a Meowy party (lobby data v=1), led by this player or a Steam friend.
		// A stranger's lobby (a join link posted anywhere) would otherwise get
		// every server this player is on.
		std::string Refusal(const char* version, uint64_t owner, uint64_t self, int ownerRelationship);
	}
}
