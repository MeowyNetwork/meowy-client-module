#define _CRT_SECURE_NO_WARNINGS   // sscanf on fixed test hex
// The signed policy (kill switch), no game and no network:
//   policy_smoke <empty scratch folder>
// Throwaway test keys and signatures made the way tools/policy/sign-policy.ps1
// signs (.NET ECDsa.SignData, SHA-256, IEEE P1363), checked through the pack's
// BCrypt code: parsing, which key signed, the ops key only making things
// stricter, replay, keeping and reading back, and what the stub reads.
// Exit code 0 when every check passes.
#include "../src/policy.h"
#include "../src/file_util.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
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

// Made by a throwaway script (keys never written anywhere, used nowhere else),
// signed as tools/policy/sign-policy.ps1 signs. launcher/core/src/policy.rs has the same.
static const char* kRelease = "ce0f613c979afd27f505b74c81bd7a2d290302978571a52b85d50b8a628e63768f48ffaab29d552511cf25016031400623cb01f1d7902e106d66d262aa66f3ff";
static const char* kOps = "23382955a362316831cf2b419eb3a6614daf64296b737ed1522a51578aaf67b00de5190d1911473e011ca6a549a5eec070f83f1799a095ae53049eedac68b075";
static const char* kRecovery = "060434149f915e4c92e5172341aef705f3b9e59e6d794fc12d3f43c0283280c781a9d6f6f8446406f42f07cf32bdc8c17a4364aeb2347bfac497d5434d72b430";

struct Signed
{
	const char* body;
	const char* sig;
};
static const Signed P1 = { "meowy-pack-policy 1\nseq 1\npack_enabled 1\noff meowy_pak_cache\n",
	"a347bd868e414cc51d08d9fc7b2813ab45afc1d38dd73f6bb78951261533c6ffc76d1e6f530788cc19116157015d0097ee0b95bc0a9dcd557bf4f5c6cecdbb1b" };
static const Signed P2 = { "meowy-pack-policy 1\nseq 2\npack_enabled 1\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\n",
	"db43a9d071a2165c03f6e65316665a01307c21893ffae0fa09ce66dfdb645bce3ed57b21305e345463e54be1971f69d92b6c739651184c4d408e67cf2b0f1001" };
static const Signed P3 = { "meowy-pack-policy 1\nseq 3\npack_enabled 1\n",
	"e8d61975cc0e9d86fc63dfe9c3d5944a8ec0bb06cae4f18acb9f656d6c547707b7563b9ecad5395c137b527bcbfeb6f50051afdfad892bbf534e998c63d2f9cb" };
static const Signed P4 = { "meowy-pack-policy 1\nseq 4\npack_enabled 0\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\n",
	"61c915601ec02c8f76ef8801d06c3f9f8789e45738be4e550cbe6fd35ff7a4db1ecb2e8e6c5f43e8fffc8876b1e2c78f7936470b84f17cff0333d4044562756e" };
// The recovery key with a notice of its own: refused.
static const Signed P4N = { "meowy-pack-policy 1\nseq 5\npack_enabled 0\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\nnotice Update at evil.example to keep playing.\n",
	"a3b2cc256ca1d66fd9d477d96f2dd512955634b3c0c28ed890475d16ce8563e9038051b58162f100274fe8533e02eb791b321e394c3f5bdefe890a408745cd2e" };
// The ops key at the largest seq there is: refused (the release key could never beat it).
static const Signed PMAX = { "meowy-pack-policy 1\nseq 9999999999999999999\npack_enabled 0\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\n",
	"45182755e76cc52910f92525b9ea2e3500061913562bc15e01f7a5671ddebb324f404c4d0bc06eca065e7d79297e706f1501d6cc9e3af17ec15bc61563e0c220" };
static const Signed P5 = { "meowy-pack-policy 1\nseq 5\npack_enabled 1\nmin_version 1.6.0\nfuture_field whatever it is\nnotice Back on.\n",
	"7b4fc4732ba87d28f0565ea104ef29d423b8c81f939709fde0fe6f4db9d7ad26a4caa01c556fcb28dabf4532cf3ad379d1680dc02af36958731c8ff3784946b1" };
static const Signed P0 = { "meowy-pack-policy 1\nseq 1\npack_enabled 0\n",
	"6a935633d866a834768a91a116aa0d7bae4ea6e784300476d8342a8a9252732de7652492090e80645dc4583d74601ea5ccf3d7307d9d37af567a70701dffeecf" };

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: policy_smoke <empty scratch folder>\n");
		return 2;
	}
	std::string arg = argv[1];
	const std::wstring dir = std::wstring(arg.begin(), arg.end()) + (sizeof(void*) == 8 ? L"\\policy_smoke64" : L"\\policy_smoke32");
	CreateDirectoryW(dir.c_str(), nullptr);
	for (const wchar_t* f : { L"policy.txt", L"policy.sig", L"policy.bin" })
		DeleteFileW((dir + L"\\" + f).c_str());

	using policy::Key;
	// The built-in production keys: the first real policy (seq 1, all on), signed
	// with the release key in the TPM, verifies; the test vectors don't.
	Check("production keys built in", policy::HaveKeys());
	Check("the real first policy: release key", policy::SignedBy("meowy-pack-policy 1\nseq 1\npack_enabled 1\n",
		"2fe5a03a75b75fefd541d0db5578fb8b77708b3a336e6038c1e83ce39a994f0a211ab51eb34b7bba73b7e5093bd336824a2401abf97dc6b951af0b5e8ac2ea46") == Key::Release);
	Check("test vectors: not production keys", policy::SignedBy(P1.body, P1.sig) == Key::None);
	const std::vector<uint8_t> release = Bytes(kRelease), ops = Bytes(kOps), recovery = Bytes(kRecovery);
	policy::detail::SetKeys(release.data(), ops.data(), recovery.data());
	Check("keys set", policy::HaveKeys());

	// Parsing.
	policy::Policy p;
	const char* why = nullptr;
	Check("parse: a policy", policy::Parse(P4N.body, &p, &why) && p.seq == 5 && !p.packEnabled && p.off.size() == 2 && p.revoked.size() == 1
		&& p.notice == "Update at evil.example to keep playing.");
	Check("parse: an unknown keyword is skipped", policy::Parse(P5.body, &p, &why) && p.minVersion == "1.6.0");
	Check("parse: wrong header", !policy::Parse("meowy-pack-policy 2\nseq 1\npack_enabled 1\n", &p, &why));
	Check("parse: no seq", !policy::Parse("meowy-pack-policy 1\npack_enabled 1\n", &p, &why));
	Check("parse: no pack_enabled", !policy::Parse("meowy-pack-policy 1\nseq 1\n", &p, &why));
	Check("parse: seq with a leading zero", !policy::Parse("meowy-pack-policy 1\nseq 01\npack_enabled 1\n", &p, &why));
	Check("parse: seq twice", !policy::Parse("meowy-pack-policy 1\nseq 1\nseq 2\npack_enabled 1\n", &p, &why));
	Check("parse: CR line ends", !policy::Parse("meowy-pack-policy 1\r\nseq 1\r\npack_enabled 1\r\n", &p, &why));
	Check("parse: no final line end", !policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1", &p, &why));
	Check("parse: a feature twice", !policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1\noff a\noff a\n", &p, &why));
	Check("parse: a bad feature name", !policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1\noff Meowy-X\n", &p, &why));
	Check("parse: a bad version", !policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1\nrevoked 1.x\n", &p, &why));
	Check("parse: a control character in the notice", !policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1\nnotice a\tb\n", &p, &why));
	Check("parse: an overlong UTF-8 notice", !policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1\nnotice \xC0\xAF\n", &p, &why));
	Check("parse: UTF-8 notice", policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1\nnotice caf\xC3\xA9\n", &p, &why));
	Check("parse: too big", !policy::Parse(std::string("meowy-pack-policy 1\nseq 1\npack_enabled 1\nnotice ") + std::string(9000, 'a') + "\n", &p, &why));

	// Which key signed it.
	Check("signed by the release key", policy::SignedBy(P1.body, P1.sig) == Key::Release);
	Check("signed by the ops key", policy::SignedBy(P2.body, P2.sig) == Key::Ops);
	Check("signed by the recovery key", policy::SignedBy(P4.body, P4.sig) == Key::Recovery);
	std::string tampered = P1.body;
	tampered[tampered.size() - 2] = 'X';
	Check("a changed byte: no key", policy::SignedBy(tampered, P1.sig) == Key::None);
	Check("another body's signature: no key", policy::SignedBy(P1.body, P3.sig) == Key::None);
	Check("a short signature: no key", policy::SignedBy(P1.body, "abcd") == Key::None);

	// Versions.
	Check("versions: 1.6.0 < 1.10", policy::CompareVersions("1.6.0", "1.10") < 0);
	Check("versions: 1.6 == 1.6.0", policy::CompareVersions("1.6", "1.6.0") == 0);
	policy::Parse(P2.body, &p, &why);
	Check("revoked version can't run", !policy::VersionAllowed(p, "1.6.1") && policy::VersionAllowed(p, "1.6.2"));
	policy::Parse(P5.body, &p, &why);
	Check("below min_version can't run", !policy::VersionAllowed(p, "1.5.9") && policy::VersionAllowed(p, "1.6.0"));
	policy::Parse(P4.body, &p, &why);
	Check("pack off: no version runs", !policy::VersionAllowed(p, "9.9"));

	// Offered in order: release, ops stricter, ops re-enabling (refused),
	// recovery off, recovery with a notice and ops at the largest seq (both
	// refused), an old one (refused), release back on.
	std::string result;
	Check("release policy taken", policy::Offer(dir, P1.body, P1.sig, result));
	Check("stub file: on", file_util::ReadSmall(dir + L"\\policy.bin", 256) == "enabled 1\nseq 1\n");
	Check("ops: stricter taken", policy::Offer(dir, P2.body, P2.sig, result));
	Check("ops: switching a feature back on refused", !policy::Offer(dir, P3.body, P3.sig, result));
	Check("recovery: switching the pack off taken", policy::Offer(dir, P4.body, P4.sig, result));
	Check("stub file: off, revoked versions", file_util::ReadSmall(dir + L"\\policy.bin", 256)
		== "enabled 0\nseq 4\nrevoked 1.6.1\n");
	Check("recovery: a notice of its own refused", !policy::Offer(dir, P4N.body, P4N.sig, result));
	Check("ops: seq 9999999999999999999 refused", !policy::Offer(dir, PMAX.body, PMAX.sig, result));
	Check("an older policy refused (no replay)", !policy::Offer(dir, P0.body, P0.sig, result));
	Check("a tampered policy refused", !policy::Offer(dir, tampered, P1.sig, result));
	policy::Policy kept;
	Check("kept: policy 4", policy::Current(dir, &kept) && kept.seq == 4 && !kept.packEnabled);
	Check("release: back on (and the ops key's off list gone)", policy::Offer(dir, P5.body, P5.sig, result));
	Check("kept: policy 5", policy::Current(dir, &kept) && kept.seq == 5 && kept.packEnabled && kept.off.empty());
	Check("stub file: the release key's notice", file_util::ReadSmall(dir + L"\\policy.bin", 256)
		== "enabled 1\nseq 5\nmin_version 1.6.0\nnotice Back on.\n");
	Check("effective: policy 5", policy::Effective().seq == 5);
	// A stub file the 1.7.0 launcher wrote (no min_version) is written again at start.
	file_util::WriteAtomic(dir + L"\\policy.bin", "enabled 1\nseq 5\nnotice Back on.\n");
	Check("stub file: healed from the kept policy", policy::HealStubFile(dir) && file_util::ReadSmall(dir + L"\\policy.bin", 256)
		== "enabled 1\nseq 5\nmin_version 1.6.0\nnotice Back on.\n");
	Check("stub file: already right, left alone", !policy::HealStubFile(dir));
	// The heal (at every start, on the game's thread) while the fetch thread takes
	// a newer policy: the stub file always ends up the newer one's.
	{
		int stale = 0;
		for (int round = 0; round < 40; ++round)
		{
			file_util::WriteAtomic(dir + L"\\policy.txt", P4.body);
			file_util::WriteAtomic(dir + L"\\policy.sig", std::string(P4.sig) + "\n");
			file_util::WriteAtomic(dir + L"\\policy.bin", "enabled 1\nseq 4\n");
			std::atomic<bool> stop{ false };
			std::thread healer([&] { while (!stop) policy::HealStubFile(dir); });
			policy::Offer(dir, P5.body, P5.sig, result);
			stop = true;
			healer.join();
			if (file_util::ReadSmall(dir + L"\\policy.bin", 256) != "enabled 1\nseq 5\nmin_version 1.6.0\nnotice Back on.\n")
				++stale;
		}
		Check("stub file: a heal racing a newer policy never puts the older one back", stale == 0);
	}

	// Edited on disk: no longer verifies, so it isn't kept.
	file_util::WriteAtomic(dir + L"\\policy.txt", "meowy-pack-policy 1\nseq 9\npack_enabled 1\n");
	Check("edited on disk: not used", !policy::Current(dir, &kept));

	// MayReplace on its own.
	policy::Policy cur, next;
	policy::Parse(P2.body, &cur, &why);
	policy::Parse("meowy-pack-policy 1\nseq 3\npack_enabled 1\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\nmin_version 1.6.0\n", &next, &why);
	Check("ops: raising min_version allowed", policy::MayReplace(cur, true, next, Key::Ops, &why));
	cur.minVersion = "1.7.0";
	Check("ops: lowering min_version refused", !policy::MayReplace(cur, true, next, Key::Ops, &why));
	Check("release: anything newer allowed", policy::MayReplace(cur, true, next, Key::Release, &why));
	Check("unsigned: refused", !policy::MayReplace(cur, true, next, Key::None, &why));

	// The seq: the ops and recovery keys stay where the release key can beat them.
	policy::Parse("meowy-pack-policy 1\nseq 9999999999999999999\npack_enabled 0\n", &next, &why);
	Check("ops: seq 9999999999999999999 refused", !policy::MayReplace(cur, true, next, Key::Ops, &why));
	Check("recovery: seq 9999999999999999999 refused", !policy::MayReplace(cur, true, next, Key::Recovery, &why));
	Check("ops: seq 9999999999999999999 refused with none kept", !policy::MayReplace(cur, false, next, Key::Ops, &why));
	Check("release: seq 9999999999999999999 allowed", policy::MayReplace(cur, true, next, Key::Release, &why));
	policy::Policy top = next;
	next.seq = 3;
	Check("release: never an older seq", !policy::MayReplace(top, true, next, Key::Release, &why));
	policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1\n", &cur, &why);
	next = cur;
	next.packEnabled = false;
	next.seq = 1 + policy::kMaxStepSeq;
	Check("ops: a step of kMaxStepSeq allowed", policy::MayReplace(cur, true, next, Key::Ops, &why));
	next.seq = 2 + policy::kMaxStepSeq;
	Check("ops: one more refused", !policy::MayReplace(cur, true, next, Key::Ops, &why));
	Check("ops: none kept, up to kMaxStepSeq", !policy::MayReplace(cur, false, next, Key::Ops, &why));
	cur.seq = policy::kReleaseOnlySeq - 2;
	next.seq = policy::kReleaseOnlySeq - 1;
	Check("ops: just below the release key's range allowed", policy::MayReplace(cur, true, next, Key::Ops, &why));
	cur.seq = policy::kReleaseOnlySeq - 1;
	next.seq = policy::kReleaseOnlySeq;
	Check("ops: the release key's range refused", !policy::MayReplace(cur, true, next, Key::Ops, &why));
	Check("release: its range allowed", policy::MayReplace(cur, true, next, Key::Release, &why));
	// The notice: the ops and recovery keys keep or drop the kept one, no new text.
	policy::Parse("meowy-pack-policy 1\nseq 1\npack_enabled 1\nnotice Maintenance tonight.\n", &cur, &why);
	next = cur;
	next.seq = 2;
	next.packEnabled = false;
	Check("ops: keeping the notice allowed", policy::MayReplace(cur, true, next, Key::Ops, &why));
	next.notice.clear();
	Check("ops: dropping it allowed", policy::MayReplace(cur, true, next, Key::Ops, &why));
	next.notice = "Download the fix at evil.example";
	Check("ops: a new notice refused", !policy::MayReplace(cur, true, next, Key::Ops, &why));
	Check("recovery: a new notice refused", !policy::MayReplace(cur, true, next, Key::Recovery, &why));
	Check("ops: a notice with none kept refused", !policy::MayReplace(cur, false, next, Key::Ops, &why));
	Check("release: a new notice allowed", policy::MayReplace(cur, true, next, Key::Release, &why));

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
