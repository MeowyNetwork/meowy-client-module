// The rejoin-after-a-crash session file, no game:
//   session_smoke <empty scratch folder>
// Exit code 0 when every check passes.

#include "../src/resilience.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

static std::string Read(const std::wstring& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv)
{
	using namespace resilience;
	Check("name kept", CleanName("Foo RP | 24/7") == "Foo RP | 24/7");
	Check("line breaks and controls dropped", CleanName("a\r\nb\tc\x01" "d") == "abcd");
	Check("capped at 64 bytes", CleanName(std::string(100, 'x')).size() == 64);
	// 63 ASCII bytes then a 2-byte character: cut before it, not through it.
	const std::string utf8 = std::string(63, 'x') + "\xc3\xa9" + "yyy";
	const std::string cut = CleanName(utf8);
	Check("a UTF-8 character isn't split", cut.size() == 63 && cut == std::string(63, 'x'));

	wchar_t full[MAX_PATH];
	std::string root = argc > 1 ? argv[1] : ".";
	GetFullPathNameW(std::wstring(root.begin(), root.end()).c_str(), MAX_PATH, full, nullptr);
	const std::wstring dir = full;
	const std::wstring file = dir + L"\\on_server.txt";

	MarkSession(dir, "1.2.3.4:27015", "Foo\r\nRP");
	const std::string text = Read(file);
	Check("file: address, time, then the cleaned name", text.rfind("1.2.3.4:27015 ", 0) == 0 && text.find("\r\nFooRP\r\n") != std::string::npos);
	MarkSession(dir, "1.2.3.4:27015 evil", "x");
	Check("an address with a space isn't written", Read(file) == text);
	std::string name = "unchanged";
	Check("no crash report since: nothing offered", CrashedSession(dir, &name).empty());
	Check("and the file is gone", GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);
	MarkSession(dir, "1.2.3.4:27015", "x");
	ClearSession(dir);
	Check("cleared on leaving", GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
