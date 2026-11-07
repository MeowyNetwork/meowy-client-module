// The pack's end of the Meowy Launcher's pipe (src/launcher_pipe.h): which
// lines it takes from the launcher and what it sends, no game:
//   launcher_pipe_smoke
// Exit code 0 when every check passes.

#include "../src/launcher_pipe.h"

#include <cstdio>
#include <string>

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
	using namespace launcher_pipe::detail;
	std::string a;

	Check("join: ours", ParseJoin("{\"t\":\"join\",\"a\":\"138.201.62.52:27015\"}", a) && a == "138.201.62.52:27015");
	Check("join: a public server", ParseJoin("{\"t\":\"join\",\"a\":\"1.2.3.4:27015\"}", a) && a == "1.2.3.4:27015");
	const char* refused[] = {
		"{\"t\":\"join\",\"a\":\"192.168.1.5:27015\"}",            // not public
		"{\"t\":\"join\",\"a\":\"127.0.0.1:27015\"}",
		"{\"t\":\"join\",\"a\":\"1.2.3.4:27015;quit\"}",           // anything but digits, dots and a colon
		"{\"t\":\"join\",\"a\":\"1.2.3.4:27015 password\"}",
		"{\"t\":\"join\",\"a\":\"1.2.3.4\"}",                      // no port
		"{\"t\":\"join\",\"a\":\"example.com:27015\"}",
		"{\"t\":\"join\", \"a\":\"1.2.3.4:27015\"}",               // not the exact form
		"{\"a\":\"1.2.3.4:27015\",\"t\":\"join\"}",
		"{\"t\":\"run\",\"a\":\"1.2.3.4:27015\"}",
		"{\"t\":\"join\",\"a\":\"1.2.3.4:27015\"}x",
		"{\"t\":\"join\",\"a\":\"\"}",
		"",
	};
	for (const char* line : refused)
	{
		a = "unchanged";
		Check(line, !ParseJoin(line, a) && a == "unchanged");
	}

	Check("state: menu has no address", StateLine("menu", "1.2.3.4:27015") == "{\"t\":\"state\",\"s\":\"menu\"}\n");
	Check("state: in game with the engine's address",
		StateLine("ingame", "138.201.62.52:27015") == "{\"t\":\"state\",\"s\":\"ingame\",\"a\":\"138.201.62.52:27015\"}\n");
	Check("state: joining", StateLine("joining", "1.2.3.4:5") == "{\"t\":\"state\",\"s\":\"joining\",\"a\":\"1.2.3.4:5\"}\n");
	Check("state: a private or odd address isn't sent", StateLine("ingame", "loopback") == "{\"t\":\"state\",\"s\":\"ingame\"}\n" &&
		StateLine("ingame", "10.0.0.1:27015") == "{\"t\":\"state\",\"s\":\"ingame\"}\n");
	Check("state: only the three", StateLine("hacked", "") == "" && StateLine("", "") == "");

	Check("hello", HelloLine("1.6.0") == "{\"t\":\"hello\",\"v\":\"1.6.0\"}\n");
	Check("hello: only digits and dots", HelloLine("1.6.0\"}\n{\"t\":\"x") == "{\"t\":\"hello\",\"v\":\"1.6.0\"}\n");

	const std::wstring exe = L"C:\\Users\\A\\AppData\\Local\\MeowyClientPack\\launcher\\MeowyLauncher.exe";
	Check("server path: same", SamePath(exe, exe));
	Check("server path: case and slashes", SamePath(L"c:/users/a/appdata/local/meowyclientpack/launcher/meowylauncher.EXE", exe));
	Check("server path: anything else", !SamePath(L"C:\\Users\\A\\Downloads\\MeowyLauncher.exe", exe) && !SamePath(L"", exe) &&
		!SamePath(exe + L"x", exe));

	// The menu Lua chunk takes only what the native side checked.
	const std::string lua = launcher_pipe::kLua;
	Check("lua: connect only with the checked join", lua.find("JoinServer(join)") != std::string::npos &&
		lua.find("RunString") == std::string::npos && lua.find("CompileString") == std::string::npos);

	launcher_pipe::SetState("ingame", "1.2.3.4:27015");
	Check("no join queued", launcher_pipe::TakeJoin().empty());
	Check("not connected (no launcher here)", !launcher_pipe::Connected());

	std::printf("%d checks, %d failed\n", g_count, g_failures);
	return g_failures == 0 ? 0 : 1;
}
