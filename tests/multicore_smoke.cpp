// Multicore rendering's native half (multicore.h), no game:
//   multicore_smoke <scratch folder>
// The managed flag the client realm reads, and the crash guard's write at a
// clean exit: written only when armed, and not when the game wrote a crash
// report during this run.
// Exit code 0 when every check passes.
#include "../src/multicore.h"
#include "../src/file_util.h"

#include <cstdio>
#include <ctime>
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

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2)
	{
		std::printf("usage: multicore_smoke <scratch folder>\n");
		return 2;
	}
	const std::wstring dir = std::wstring(argv[1]) + L"\\multicore_smoke";
	const std::wstring root = dir + L"\\game";
	const std::wstring path = dir + L"\\multicore_guard.txt";
	CreateDirectoryW(dir.c_str(), nullptr);
	CreateDirectoryW(root.c_str(), nullptr);
	DeleteFileW(path.c_str());
	DeleteFileW((root + L"\\crashes\\smoke.txt").c_str());
	const long long now = static_cast<long long>(std::time(nullptr));

	// The flag: off until the menu says otherwise.
	Check("managed: off at first", !multicore::Managed());
	multicore::SetManaged(true);
	Check("managed: on", multicore::Managed());
	multicore::SetManaged(false);
	Check("managed: off again", !multicore::Managed());

	// Nothing armed: nothing written.
	Check("exit: nothing armed, nothing written", !multicore::AtProcessExit());
	Check("exit: no file", file_util::ReadSmall(path, 4096).empty());

	// Armed, no crash report: the text is written, once.
	multicore::ArmExitWrite(path, "running=0\nmode=on\n", root, now - 60);
	Check("exit: armed, written", multicore::AtProcessExit());
	Check("exit: the text", file_util::ReadSmall(path, 4096) == "running=0\nmode=on\n");
	Check("exit: only once", !multicore::AtProcessExit());

	// The latest text wins.
	multicore::ArmExitWrite(path, "first\n", root, now - 60);
	multicore::ArmExitWrite(path, "second\n", root, now - 60);
	Check("exit: latest text", multicore::AtProcessExit() && file_util::ReadSmall(path, 4096) == "second\n");

	// Disarmed: nothing.
	multicore::ArmExitWrite(path, "third\n", root, now - 60);
	multicore::Disarm();
	Check("exit: disarmed, nothing written", !multicore::AtProcessExit());
	Check("exit: disarmed, file kept", file_util::ReadSmall(path, 4096) == "second\n");

	// A crash report written during this run: not a clean exit.
	CreateDirectoryW((root + L"\\crashes").c_str(), nullptr);
	Check("crash: report made", file_util::WriteAtomic(root + L"\\crashes\\smoke.txt", "crash\n", file_util::Durability::Relaxed));
	multicore::ArmExitWrite(path, "fourth\n", root, now - 60);
	Check("crash: this run's report, nothing written", !multicore::AtProcessExit());
	Check("crash: file kept", file_util::ReadSmall(path, 4096) == "second\n");

	// A report from before this run doesn't count.
	multicore::ArmExitWrite(path, "fifth\n", root, now + 3600);
	Check("crash: an older report, written", multicore::AtProcessExit());
	Check("crash: older report, the text", file_util::ReadSmall(path, 4096) == "fifth\n");

	// No crash folder known: written.
	multicore::ArmExitWrite(path, "sixth\n", L"", now - 60);
	Check("no root: written", multicore::AtProcessExit() && file_util::ReadSmall(path, 4096) == "sixth\n");

	// The guard's evidence at the next start (CrashSince): a report written in
	// the session (after its start, before this process's start) counts.
	Check("evidence: a report in the session", multicore::CrashSince(root, now - 60, now + 60));
	Check("evidence: a report in the same second as the start", multicore::CrashSince(root, now, now + 60));
	Check("evidence: the session started after the report", !multicore::CrashSince(root, now + 3600, now + 7200));
	Check("evidence: the report is from after this process started", !multicore::CrashSince(root, now - 3600, now - 60));
	Check("evidence: unknown start", !multicore::CrashSince(root, 0, now + 60));
	Check("evidence: no root", !multicore::CrashSince(L"", now - 60, now + 60));
	DeleteFileW((root + L"\\crashes\\smoke.txt").c_str());
	Check("evidence: no report", !multicore::CrashSince(root, now - 60, now + 60));

	DeleteFileW((root + L"\\crashes\\smoke.txt").c_str());
	RemoveDirectoryW((root + L"\\crashes").c_str());
	RemoveDirectoryW(root.c_str());
	DeleteFileW(path.c_str());
	RemoveDirectoryW(dir.c_str());

	std::printf("%d checks, %d failed\n", g_count, g_failures);
	return g_failures == 0 ? 0 : 1;
}
