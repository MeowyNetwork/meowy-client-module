#define _CRT_SECURE_NO_WARNINGS   // sscanf on fixed test hex
// The signed updater, no game and no network:
//   updater_smoke <empty scratch folder>
// Release documents (parsing; only the release key's signature counts), who
// takes one (seq, expiry, version, bad list, policy, stub, rollout), and the
// stub's choices over scratch files (promote, verify, policy, roll back after a
// crash, the installed copy as the floor). Exit code 0 when every check passes.
#include "../src/updater.h"
#include "../src/pack_versions.h"
#include "../src/policy.h"
#include "../src/file_util.h"

#include <cstdio>
#include <string>
#include <vector>

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

static std::vector<uint8_t> Bytes(const char* hex)
{
	std::vector<uint8_t> out;
	for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
	{
		unsigned v = 0;
		std::sscanf(hex + i, "%2x", &v);
		out.push_back(static_cast<uint8_t>(v));
	}
	return out;
}

static std::string Repeat(const char* s, int n)
{
	std::string out;
	for (int i = 0; i < n; ++i)
		out += s;
	return out;
}

static void Wipe(const std::wstring& dir)
{
	WIN32_FIND_DATAW f;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			const std::wstring name = f.cFileName;
			if (name == L"." || name == L"..")
				continue;
			if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				Wipe(dir + L"\\" + name);
			else
				DeleteFileW((dir + L"\\" + name).c_str());
		} while (FindNextFileW(h, &f));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// A fake pack version in dataDir\bin\<v>, and its hash.
static std::string PutVersion(const std::wstring& dataDir, const std::string& v, const std::string& content)
{
	CreateDirectoryW((dataDir + L"\\bin").c_str(), nullptr);
	CreateDirectoryW((dataDir + L"\\bin\\" + std::wstring(v.begin(), v.end())).c_str(), nullptr);
	const std::wstring dll = pack_versions::VersionDll(dataDir, v);
	file_util::WriteAtomic(dll, content);
	return pack_versions::Sha256File(dll);
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: updater_smoke <empty scratch folder>\n");
		return 2;
	}
	std::string arg = argv[1];
	const std::wstring dataDir = std::wstring(arg.begin(), arg.end()) + (sizeof(void*) == 8 ? L"\\updater_smoke64" : L"\\updater_smoke32");
	Wipe(dataDir);
	CreateDirectoryW(dataDir.c_str(), nullptr);
	CreateDirectoryW((dataDir + L"\\state").c_str(), nullptr);

	// --- Release documents ------------------------------------------------------------
	const std::string body = "meowy-pack-release 1\nseq 3\nversion 1.7.0\nexpires 4102444800\nmin_stub 1\nrollout 100\nsalt 0123abcd\n"
		"file win64 1048576 " + Repeat("ab", 32) + " /pack/v1/bin/1.7.0/gmcl_meowy_win64.dll\n"
		"file win32 786432 " + Repeat("cd", 32) + " /pack/v1/bin/1.7.0/gmcl_meowy_win32.dll\n";
	updater::Release r;
	const char* why = nullptr;
	Check("parse: a release", updater::Parse(body, &r, &why) && r.seq == 3 && r.version == "1.7.0" && r.files.size() == 2 && r.rollout == 100);
	Check("parse: own file", updater::OwnFile(r) != nullptr && updater::OwnFile(r)->arch == (sizeof(void*) == 8 ? "win64" : "win32"));
	auto bad = [&](const std::string& b) { return !updater::Parse(b, nullptr, &why); };
	Check("parse: wrong header", bad("meowy-pack-release 2\n" + body.substr(body.find('\n') + 1)));
	Check("parse: no files", bad("meowy-pack-release 1\nseq 3\nversion 1.7.0\nexpires 4102444800\nmin_stub 1\nrollout 100\nsalt 0123abcd\n"));
	Check("parse: a path off the site's pack folder", bad("meowy-pack-release 1\nseq 3\nversion 1.7.0\nexpires 4102444800\nmin_stub 1\nrollout 100\nsalt 0123abcd\nfile win64 10 " + Repeat("ab", 32) + " /other/x.dll\n"));
	Check("parse: a path climbing out", bad("meowy-pack-release 1\nseq 3\nversion 1.7.0\nexpires 4102444800\nmin_stub 1\nrollout 100\nsalt 0123abcd\nfile win64 10 " + Repeat("ab", 32) + " /pack/v1/../x.dll\n"));
	Check("parse: a full URL is no path", bad("meowy-pack-release 1\nseq 3\nversion 1.7.0\nexpires 4102444800\nmin_stub 1\nrollout 100\nsalt 0123abcd\nfile win64 10 " + Repeat("ab", 32) + " https://evil.example/x.dll\n"));
	Check("parse: an uppercase hash", bad("meowy-pack-release 1\nseq 3\nversion 1.7.0\nexpires 4102444800\nmin_stub 1\nrollout 100\nsalt 0123abcd\nfile win64 10 " + Repeat("AB", 32) + " /pack/v1/x.dll\n"));
	Check("parse: rollout over 100", bad("meowy-pack-release 1\nseq 3\nversion 1.7.0\nexpires 4102444800\nmin_stub 1\nrollout 101\nsalt 0123abcd\nfile win64 10 " + Repeat("ab", 32) + " /pack/v1/x.dll\n"));
	Check("parse: a file twice", bad(body + "file win64 10 " + Repeat("ab", 32) + " /pack/v1/x.dll\n"));
	Check("parse: an unknown keyword is skipped", updater::Parse(body + "channel_note whatever\n", nullptr, &why));

	// Only the release key signs a release (the ops key can't, policy.h).
	const std::vector<uint8_t> testRelease = Bytes("076beed76cb2436931a8277c91ce64bb851a8ab806cc6b690617e400174d75c38bef6d86bd201865469eff8f3ae222460ce73177d68746b7d389a337e662cf10");
	const std::vector<uint8_t> testOps = Bytes("9dc2557d2fe422674364d7b681d9e1ee2d648ef6e6063a98a7dcd59f8e72d92ba63681ffc4b21076f83e2dacf7b393bd2f21ff5b343b98711ef33429975d29e4");
	policy::detail::SetKeys(testRelease.data(), testOps.data(), nullptr);
	Check("signed by the release key", policy::SignedBy(body, "396fde6b2f5c94c6ad515ede6d03d5527bd104cc2aab2fdbc405f8dd5b0ba0fd0591a1e450a961cea1ac58c1c6717ce0c566e54759d65896962f5862b57d1da8") == policy::Key::Release);
	Check("signed by the ops key: not a release key", policy::SignedBy(body, "bcfc278c3cdd2a3763dfc7463f38afd67dd4fcfd739fba0e241ae1b3f507e4e79bf5948b48bf6962e437977284ed925cc162c32288ea4b902600915744349d26") == policy::Key::Ops);

	// --- Who takes it -------------------------------------------------------------------
	updater::Facts f;
	f.running = "1.6.0";
	f.stub = 1;
	f.lastSeq = 2;
	f.now = 1800000000;
	f.token = Repeat("0f", 16);
	std::string reason;
	Check("take: a newer version", updater::ShouldTake(r, f, reason) && reason == "update to 1.7.0");
	updater::Facts g = f;
	g.lastSeq = 3;
	Check("take: not a seq already taken (no replay)", !updater::ShouldTake(r, g, reason));
	g = f;
	g.now = 4102444800;
	Check("take: not once expired", !updater::ShouldTake(r, g, reason));
	g = f;
	g.running = "1.7.0";
	Check("take: not the same version", !updater::ShouldTake(r, g, reason));
	g.running = "1.8.0";
	Check("take: never a downgrade", !updater::ShouldTake(r, g, reason));
	g = f;
	g.bad = { "1.7.0" };
	Check("take: not a version that failed here", !updater::ShouldTake(r, g, reason));
	g = f;
	g.policyAllows = false;
	Check("take: not against the policy", !updater::ShouldTake(r, g, reason));
	g = f;
	g.stub = 0;
	Check("take: not with an old stub", !updater::ShouldTake(r, g, reason));
	updater::Release r2 = r;
	r2.rollout = 0;
	Check("take: rollout 0, nobody", !updater::ShouldTake(r2, f, reason));
	int in = 0;
	for (int i = 0; i < 400; ++i)
		in += updater::RolloutSlot("token" + std::to_string(i), r.salt) < 50 ? 1 : 0;
	Check("rollout 50: about half the installs", in > 150 && in < 250);
	Check("rollout slot: the same every time", updater::RolloutSlot(f.token, r.salt) == updater::RolloutSlot(f.token, r.salt));

	// --- The stub's choices --------------------------------------------------------------
	Check("no pointers: the installed copy", pack_versions::Choose(dataDir).path.empty());
	pack_versions::Pointer p;
	p.version = "1.7.0";
	p.sha256 = PutVersion(dataDir, "1.7.0", "version 1.7.0");
	pack_versions::WritePointer(pack_versions::StatePath(dataDir, L"pending"), p);
	// The player's switch (menu_settings.txt, as the menu Lua writes it).
	const std::wstring settings = dataDir + L"\\menu_settings.txt";
	Check("updates off: no settings file means on", !pack_versions::UpdatesOff(dataDir));
	file_util::WriteAtomic(settings, "meowy_loadingscreen=1\nmeowy_updates=1\n");
	Check("updates off: 1 is on", !pack_versions::UpdatesOff(dataDir));
	file_util::WriteAtomic(settings, "meowy_loadingscreen=1\r\nmeowy_updates=0\r\n");
	Check("updates off: 0 is off", pack_versions::UpdatesOff(dataDir));
	file_util::WriteAtomic(settings, "meowy_updates_extra=0\n");
	Check("updates off: only its own name", !pack_versions::UpdatesOff(dataDir));
	DeleteFileW(settings.c_str());

	Check("promote: pending becomes current", pack_versions::Promote(dataDir) == "updated to 1.7.0");
	Check("promote: pending gone", GetFileAttributesW(pack_versions::StatePath(dataDir, L"pending").c_str()) == INVALID_FILE_ATTRIBUTES);
	pack_versions::Choice c = pack_versions::Choose(dataDir);
	Check("choose: the update", c.version == "1.7.0" && c.path == pack_versions::VersionDll(dataDir, "1.7.0"));

	file_util::WriteAtomic(pack_versions::VersionDll(dataDir, "1.7.0"), "tampered");
	c = pack_versions::Choose(dataDir);
	Check("choose: a changed file is passed over", c.path.empty() && c.why.find("doesn't match") != std::string::npos);
	PutVersion(dataDir, "1.7.0", "version 1.7.0");

	file_util::WriteAtomic(dataDir + L"\\state\\policy.bin", "enabled 1\nseq 9\nrevoked 1.7.0\n");
	c = pack_versions::Choose(dataDir);
	Check("choose: a revoked version is passed over", c.path.empty() && c.why.find("policy") != std::string::npos);
	file_util::WriteAtomic(dataDir + L"\\state\\policy.bin", "enabled 1\nseq 10\nmin_version 1.8\n");
	Check("choose: below min_version is passed over", pack_versions::Choose(dataDir).path.empty());
	DeleteFileW((dataDir + L"\\state\\policy.bin").c_str());

	// 1.8.0 arrives; previous is 1.7.0. It dies while starting: back to 1.7.0, 1.8.0 bad.
	p.version = "1.8.0";
	p.sha256 = PutVersion(dataDir, "1.8.0", "version 1.8.0");
	pack_versions::WritePointer(pack_versions::StatePath(dataDir, L"pending"), p);
	Check("promote: 1.8.0", pack_versions::Promote(dataDir) == "updated to 1.8.0");
	std::string said;
	Check("crash on a fresh update: reverted", pack_versions::RevertAfterCrash(dataDir, "1.8.0", said) && said.find("went back to 1.7.0") != std::string::npos);
	Check("crash: 1.8.0 marked bad", pack_versions::IsBad(dataDir, "1.8.0"));
	Check("crash: 1.7.0 chosen again", pack_versions::Choose(dataDir).version == "1.7.0");
	pack_versions::WritePointer(pack_versions::StatePath(dataDir, L"pending"), p);
	Check("a bad version isn't taken again", pack_versions::Promote(dataDir).find("failed before") != std::string::npos);

	// A version that has got through twice: a crash isn't blamed on it.
	pack_versions::NoteLoaded(dataDir, "1.7.0");
	pack_versions::NoteLoaded(dataDir, "1.7.0");
	Check("crash after two good starts: just skipped once", !pack_versions::RevertAfterCrash(dataDir, "1.7.0", said));
	Check("crash of the installed copy: just skipped once", !pack_versions::RevertAfterCrash(dataDir, "installed", said));

	// No previous left: the installed copy.
	DeleteFileW(pack_versions::StatePath(dataDir, L"current").c_str());
	DeleteFileW(pack_versions::StatePath(dataDir, L"previous").c_str());
	p.version = "1.9.0";
	p.sha256 = PutVersion(dataDir, "1.9.0", "version 1.9.0");
	pack_versions::WritePointer(pack_versions::StatePath(dataDir, L"pending"), p);
	pack_versions::Promote(dataDir);
	Check("crash with nothing before it: the installed copy", pack_versions::RevertAfterCrash(dataDir, "1.9.0", said)
		&& said.find("installed copy") != std::string::npos && pack_versions::Choose(dataDir).path.empty());

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
