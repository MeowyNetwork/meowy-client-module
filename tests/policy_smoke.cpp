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

// tools: scratchpad policy_vectors.ps1 (throwaway keys, never used anywhere else).
static const char* kRelease = "f530598b2c2aa7fb0aa05a1a33f7fb471ea56b878e82e83e4c9290a2181b2ba38a29edf32476d150c86b66821d5416c12294be78bf44550ece9c1065322efb9f";
static const char* kOps = "e0d84f65f3589c5451c976af9da54fa32826806ee092067ea619f33e9bfaf89a9a920435803d2b33c4b492dc18b2ffa66dd998bc9afc2c74e6dc77981bf4e86c";
static const char* kRecovery = "78a09ae8854b931eb4a3782ab0726000a8a900407dd61fac7631a1bc0492d9ae19f81f09013c1118ed26f55108888307cae65bd839608f055374155a0d7e4ce1";

struct Signed
{
	const char* body;
	const char* sig;
};
static const Signed P1 = { "meowy-pack-policy 1\nseq 1\npack_enabled 1\noff meowy_pak_cache\n",
	"90f879c83ad608f679e2f66797cb132331ee464b3b0d65160cc0391a8d72c8a989e37fdae652378a80620aeeaf6b27a63f7a924d0ea39c7d9207d3b8bd515dcb" };
static const Signed P2 = { "meowy-pack-policy 1\nseq 2\npack_enabled 1\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\n",
	"da20f19618897f98418697809c8f036f8eff4c01a74969f55ff100037158d135b3e438ac8d6fa50122fe8f165ca3735d8f6b04799317b3dac1918241fdcf108d" };
static const Signed P3 = { "meowy-pack-policy 1\nseq 3\npack_enabled 1\n",
	"e4235645efbf4614c326607080d3406b1ef0a13e7625b730e20ae5d909aa5e68382b76bff8cd666327bef0ae96f87b081d8d90262dc8900f6c6a71b16c60e44e" };
static const Signed P4 = { "meowy-pack-policy 1\nseq 4\npack_enabled 0\noff meowy_pak_cache\noff meowy_prewarm\nrevoked 1.6.1\nnotice Paused while we fix a crash.\n",
	"18eff360838711746675f7e14aed6ae91ee445c446d0a21020f2270986451829d90cf172100eb173b595889956687f6544dace6726f82fa8f53832a883a30307" };
static const Signed P5 = { "meowy-pack-policy 1\nseq 5\npack_enabled 1\nmin_version 1.6.0\nfuture_field whatever it is\n",
	"571e28c3eccf93d369610fdb8e8fa2a04909f527dce518cfeb1c88305c10511a4f41e975e715eabf6e6d58ee4dec4ccc0edfa8d11185870002cf8a6ce37fd93d" };
static const Signed P0 = { "meowy-pack-policy 1\nseq 1\npack_enabled 0\n",
	"d7a6a9d05f486210845134f3fc7587cf3858b0b813152c9c1ab071cab97d89e7a55463c3187416985053646ee77cc0d82160518dd63f52edbec5e0fcdc1ad492" };

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
	Check("no keys: nothing verifies", policy::SignedBy(P1.body, P1.sig) == Key::None && !policy::HaveKeys());
	const std::vector<uint8_t> release = Bytes(kRelease), ops = Bytes(kOps), recovery = Bytes(kRecovery);
	policy::detail::SetKeys(release.data(), ops.data(), recovery.data());
	Check("keys set", policy::HaveKeys());

	// Parsing.
	policy::Policy p;
	const char* why = nullptr;
	Check("parse: a policy", policy::Parse(P4.body, &p, &why) && p.seq == 4 && !p.packEnabled && p.off.size() == 2 && p.revoked.size() == 1
		&& p.notice == "Paused while we fix a crash.");
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
	// recovery off, an old one (refused), release back on.
	std::string result;
	Check("release policy taken", policy::Offer(dir, P1.body, P1.sig, result));
	Check("stub file: on", file_util::ReadSmall(dir + L"\\policy.bin", 256) == "enabled 1\nseq 1\n");
	Check("ops: stricter taken", policy::Offer(dir, P2.body, P2.sig, result));
	Check("ops: switching a feature back on refused", !policy::Offer(dir, P3.body, P3.sig, result));
	Check("recovery: switching the pack off taken", policy::Offer(dir, P4.body, P4.sig, result));
	Check("stub file: off, with the notice", file_util::ReadSmall(dir + L"\\policy.bin", 256) == "enabled 0\nseq 4\nnotice Paused while we fix a crash.\n");
	Check("an older policy refused (no replay)", !policy::Offer(dir, P0.body, P0.sig, result));
	Check("a tampered policy refused", !policy::Offer(dir, tampered, P1.sig, result));
	policy::Policy kept;
	Check("kept: policy 4", policy::Current(dir, &kept) && kept.seq == 4 && !kept.packEnabled);
	Check("release: back on (and the ops key's off list gone)", policy::Offer(dir, P5.body, P5.sig, result));
	Check("kept: policy 5", policy::Current(dir, &kept) && kept.seq == 5 && kept.packEnabled && kept.off.empty());
	Check("effective: policy 5", policy::Effective().seq == 5);

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

	std::printf("%d/%d checks passed\n", g_count - g_failures, g_count);
	return g_failures == 0 ? 0 : 1;
}
