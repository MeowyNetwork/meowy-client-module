// Holding the mouse inside the game, no game (and no clipping of this PC's cursor):
//   mouse_confine_smoke
// When it clips: only on, game in front, not minimised or hung, cursor hidden.
// Exit code 0 when every check passes.
#include "../src/mouse_confine.h"

#include <cstdio>

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
	using mouse_confine::detail::ShouldClip;
	Check("aiming in the game: held", ShouldClip(true, true, false, false, false));
	Check("off: never", !ShouldClip(false, true, false, false, false));
	Check("another window in front: free", !ShouldClip(true, false, false, false, false));
	Check("minimised: free", !ShouldClip(true, true, true, false, false));
	Check("the game hangs: free", !ShouldClip(true, true, false, true, false));
	Check("a menu shows the cursor: free", !ShouldClip(true, true, false, false, true));
	Check("off describes itself", mouse_confine::Describe() == "off");
	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
