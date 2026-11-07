// Server lists from Steam, no game: server_query_smoke
// Which master-server filters may be passed on, and that Start refuses the
// rest (and, with no Steam in this process, says so). Steam's struct layout
// is checked at compile time in server_query.cpp.
// Exit code 0 when every check passes.

#include "../src/server_query.h"

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
	using server_query::detail::FilterAllowed;
	Check("gamedir", FilterAllowed("gamedir", "garrysmod"));
	Check("has players", FilterAllowed("hasplayers", "1"));
	Check("not full", FilterAllowed("notfull", "1"));
	Check("map", FilterAllowed("map", "rp_downtown_tits_v2"));
	Check("name with a wildcard", FilterAllowed("name_match", "*Meowy*"));
	Check("an unknown key", !FilterAllowed("appid", "4000"));
	Check("an address filter isn't passed on", !FilterAllowed("gameaddr", "1.2.3.4"));
	Check("an empty value", !FilterAllowed("map", ""));
	Check("a backslash (the master-server separator)", !FilterAllowed("map", "a\\gamedir\\x"));
	Check("a control character", !FilterAllowed("name_match", std::string("a\nb")));
	Check("a long value", !FilterAllowed("name_match", std::string(128, 'a')));
	Check("127 characters is fine", FilterAllowed("name_match", std::string(127, 'a')));

	std::string why;
	Check("a refused filter refuses the list", server_query::Start(server_query::List::Internet, { { "appid", "1" } }, why) == 0
		&& why.find("not allowed") != std::string::npos);
	server_query::Filters many(17, { "hasplayers", "1" });
	Check("too many filters", server_query::Start(server_query::List::Internet, many, why) == 0 && why == "too many filters");
	Check("no Steam here: refused, with a reason", server_query::Start(server_query::List::Friends, {}, why) == 0
		&& why.find("Steam") != std::string::npos);
	bool finished = false;
	int failed = -1;
	Check("taking from an unknown list", server_query::Take(42, &finished, &failed).empty() && finished && failed == 0);
	server_query::Cancel(42);
	Check("status", server_query::Describe().find("0 lists open") == 0);

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
