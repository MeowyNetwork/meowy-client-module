// The console capture's lines, no game: console_capture_smoke
// Pieces assembled into lines, severity and colour, UTF-8 cleaning, the ring,
// reading after an id, and that Install fails cleanly without tier0.
// Exit code 0 when every check passes.

#include "../src/console_capture.h"

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
	using namespace console_capture;
	detail::Reset();
	uint64_t next = 0;

	detail::Feed(0, "Hello ", 0x112233);
	Check("no line before its newline", Read(0, 100, &next).empty() && next == 1);
	detail::Feed(1, "world\nsecond", 0x445566);
	auto lines = Read(0, 100, &next);
	Check("a line from two pieces", lines.size() == 1 && lines[0].text == "Hello world");
	Check("the first piece's colour", lines.size() == 1 && lines[0].color == 0x112233);
	Check("the worst piece's severity", lines.size() == 1 && lines[0].severity == 1);
	Check("ids from 1", lines.size() == 1 && lines[0].id == 1 && next == 2);
	Check("a time", lines.size() == 1 && lines[0].timeMs > 1600000000000LL);
	detail::Feed(0, " part\r\n", 0xFFFFFFFF);
	lines = Read(1, 100, &next);
	Check("only lines after the id", lines.size() == 1 && lines[0].text == "second part");
	Check("a carriage return dropped", lines.size() == 1 && lines[0].text.back() == 't');
	Check("its own colour from the first piece", lines.size() == 1 && lines[0].color == 0x445566);

	// UTF-8: a cut emoji and Latin-1 become U+FFFD; whole characters stay.
	detail::Feed(0, "cut \xF0\x9F\x98 whole \xF0\x9F\x98\xBA caf\xE9\n", 0xFFFFFFFF);
	lines = Read(2, 100, &next);
	Check("invalid bytes replaced", lines.size() == 1
		&& lines[0].text == "cut \xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD whole \xF0\x9F\x98\xBA caf\xEF\xBF\xBD");
	Check("clean text unchanged", detail::CleanUtf8("plain \xC3\xA9") == "plain \xC3\xA9");
	Check("an overlong form replaced", detail::CleanUtf8("\xC0\xAF") == "\xEF\xBF\xBD\xEF\xBF\xBD");

	// A long line is cut, not grown without end.
	detail::Feed(0, (std::string(kMaxLineBytes + 100, 'a') + "\n").c_str(), 0xFFFFFFFF);
	lines = Read(3, 100, &next);
	Check("a long line cut", lines.size() == 1 && lines[0].text.size() == kMaxLineBytes);

	// The ring keeps the newest kMaxLines; reading asks for at most `max`.
	detail::Reset();
	for (size_t i = 0; i < kMaxLines + 25; ++i)
		detail::Feed(0, ("line " + std::to_string(i) + "\n").c_str(), 0xFFFFFFFF);
	lines = Read(0, kMaxLines + 100, &next);
	Check("the ring keeps kMaxLines", lines.size() == kMaxLines);
	Check("the oldest dropped", !lines.empty() && lines.front().text == "line 25");
	lines = Read(0, 10, &next);
	Check("at most max: the newest", lines.size() == 10 && lines.back().text == "line " + std::to_string(kMaxLines + 24));
	Check("next id", next == kMaxLines + 26);
	Check("nothing new", Read(next - 1, 10, &next).empty());

	std::string why;
	Check("no tier0 here: refused with a reason", !Install(why) && !why.empty() && !Installed());
	Check("status", Describe().find("not hooked") == 0);

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
