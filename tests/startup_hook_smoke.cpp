// startup_hook's text rules and file round trip, no game:
//   startup_hook_smoke
// Exit code 0 when every check passes.

#include "../src/startup_hook.h"
#include "../src/server_gate.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

int main()
{
	using namespace startup_hook;
	const std::string line = kLine;

	// Stock menu.lua ends without a line break (as Garry's Mod ships it).
	const std::string stock = "include( \"menu/menu.lua\" )";
	const std::string with = WithLine(stock);
	Check("adds the line on its own line", with == stock + "\r\n" + line + "\r\n");
	Check("adds it once", WithLine(with) == with);
	Check("found", HasLine(with) && !HasLine(stock));
	Check("removed: back to stock", WithoutLine(with) == stock);

	// A file that already ends in CRLF.
	const std::string crlf = "a\r\nb\r\n";
	Check("CRLF file: no blank line", WithLine(crlf) == crlf + line + "\r\n");

	// The installer's v1 line counts as present and is removed too.
	const std::string v1 = stock + "\r\npcall(require, \"meowy_boot\") -- meowy-client-pack v1 (old)\r\n";
	Check("v1 line: present", HasLine(v1));
	Check("v1 line: not added twice", WithLine(v1) == v1);
	Check("v1 line: removed", WithoutLine(v1) == stock);

	// Healing is gated on our server (server_gate): only its real port, not the
	// throwaway test servers on the same host.
	Check("gate: Meowy Network", server_gate::IsOurAddress("138.201.62.52:27015"));
	Check("gate: same host, other port", !server_gate::IsOurAddress("138.201.62.52:27016"));
	Check("gate: no port", !server_gate::IsOurAddress("138.201.62.52"));
	Check("gate: other host", !server_gate::IsOurAddress("1.2.3.4:27015"));

#ifdef _WIN32
	// File round trip in a temporary garrysmod folder.
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring root = std::wstring(tmp) + L"meowy_startup_hook_smoke";
	const std::wstring data = root + L"\\data";
	CreateDirectoryW(root.c_str(), nullptr);
	CreateDirectoryW((root + L"\\lua").c_str(), nullptr);
	CreateDirectoryW((root + L"\\lua\\includes").c_str(), nullptr);
	CreateDirectoryW(data.c_str(), nullptr);
	const std::wstring menu = root + L"\\lua\\includes\\menu.lua";
	{
		std::ofstream f(menu, std::ios::binary);
		f << stock;
	}
	DeleteFileW((data + L"\\startup_off.txt").c_str());

	Check("heal: adds when missing", Heal(root, data) == "added");
	Check("heal: then present", Heal(root, data) == "present" && Present(root));
	Check("off: removes the line", Set(root, data, false) && !Present(root) && Off(data));
	Check("heal: respects off", Heal(root, data) == "off" && !Present(root));
	Check("on: adds it back", Set(root, data, true) && Present(root) && !Off(data));
	std::ifstream in(menu, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	Check("file: stock text kept", ss.str() == with);
	in.close();

	DeleteFileW(menu.c_str());
	DeleteFileW((data + L"\\startup_off.txt").c_str());
	RemoveDirectoryW((root + L"\\lua\\includes").c_str());
	RemoveDirectoryW((root + L"\\lua").c_str());
	RemoveDirectoryW(data.c_str());
	RemoveDirectoryW(root.c_str());
#endif

	std::printf("%d checks, %d failed\n", g_count, g_failures);
	return g_failures == 0 ? 0 : 1;
}
