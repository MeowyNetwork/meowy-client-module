// Content platform adverts, no game: content_advert_smoke [a.b.c.d:port]
// Reading the `meowy_content` rule out of A2S_RULES replies built here (a
// normal one, a missing rule, hostile ones). With an address, it also asks
// that server for real and prints what it found (not a pass/fail check: the
// server may have no advert). Exit code 0 when every check passes.

#include "../src/content_advert.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

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

int main(int argc, char** argv)
{
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
