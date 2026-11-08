// party_smoke: parties' rules (src/party_steam.h) without the game or Steam.
//   party_smoke
// Covers reading a lobby's SteamID (only a lobby's: universe 1, chat type),
// "+connect_lobby <id>" on the command line Steam starts the game with from an
// invite, which lobbies are kept once entered (a Meowy party led by a friend),
// and that without Garry's Mod's steam_api in the process every call
// says no, loads nothing and never faults.
#include "../src/party_steam.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

static int g_failed = 0;
static void Check(bool ok, const char* what)
{
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		++g_failed;
}

int main()
{
	using namespace party_steam;
	// 0x0186_0000_0123_4567: universe 1, type 8 (chat), lobby instance flags.
	const unsigned long long lobby = 0x0186000001234567ull;
	const std::string text = std::to_string(lobby);
	Check(detail::ParseLobbyId(text) == lobby, "a lobby's SteamID is read");
	Check(detail::ParseLobbyId("76561198000000000") == 0, "a player's SteamID isn't a lobby");
	Check(detail::ParseLobbyId("") == 0, "empty");
	Check(detail::ParseLobbyId("12a") == 0, "not digits");
	Check(detail::ParseLobbyId("123456789012345678901") == 0, "too long");
	Check(detail::ParseLobbyId("-" + text) == 0, "no sign");

	const std::wstring wide(text.begin(), text.end());
	Check(detail::LobbyFromCommandLine(L"\"gmod.exe\" -steam +connect_lobby " + wide + L" -condebug") == lobby, "+connect_lobby on the command line");
	Check(detail::LobbyFromCommandLine(L"gmod.exe +connect_lobby  \"" + wide + L"\"") == lobby, "quoted, extra spaces");
	Check(detail::LobbyFromCommandLine(L"gmod.exe +connect 1.2.3.4:27015") == 0, "a server join isn't a party invite");
	Check(detail::LobbyFromCommandLine(L"gmod.exe +connect_lobby 76561198000000000") == 0, "only a lobby's id");
	Check(detail::LobbyFromCommandLine(L"gmod.exe") == 0, "nothing");

	// A lobby just entered: only a Meowy party, led by this player or a Steam friend.
	const unsigned long long me = 76561198000000001ull, friendId = 76561198000000002ull, stranger = 76561198000000003ull;
	Check(detail::Refusal("1", friendId, me, detail::kFriend).empty(), "a friend's party is kept");
	Check(detail::Refusal("1", me, me, 0).empty(), "one's own party is kept");
	Check(!detail::Refusal("1", stranger, me, 0).empty(), "a stranger's party is left");
	Check(!detail::Refusal("1", stranger, me, 2).empty(), "a friend request isn't a friend");
	Check(!detail::Refusal("2", friendId, me, detail::kFriend).empty(), "another kind of lobby is left");
	Check(!detail::Refusal(nullptr, friendId, me, detail::kFriend).empty(), "a lobby without the pack's data is left");
	Check(!detail::Refusal("1", 0, me, detail::kFriend).empty(), "no owner known: left");

	// No steam_api in this process: everything says no, nothing is loaded.
	Check(GetModuleHandleW(L"steam_api64.dll") == nullptr && GetModuleHandleW(L"steam_api.dll") == nullptr, "no Steam here");
	Check(!Unavailable().empty(), "unavailable, with a reason");
	Start();
	Check(!Create(8), "no party without Steam");
	Check(!Join(lobby), "no join without Steam");
	Check(!OpenInviteDialog() && !Invite(76561198000000000ull), "no invites without Steam");
	Check(!InviteToServer(76561198000000000ull, "1.2.3.4:27015"), "no server invites without Steam");
	Check(!SetLobbyData("srv", "1.2.3.4:27015") && !SetMemberData("at", "") && !SetPresence("meowy_pack", "1"), "no data without Steam");
	Check(Read().lobby == 0 && Poll().empty() && Friends(10).empty(), "no state without Steam");
	Leave();
	Check(GetModuleHandleW(L"steam_api64.dll") == nullptr && GetModuleHandleW(L"steam_api.dll") == nullptr, "still nothing loaded");
	std::printf("%s\n", Describe().c_str());

	std::printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
	return g_failed ? 1 : 0;
}
