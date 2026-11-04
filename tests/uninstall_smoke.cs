// uninstall_smoke: the installer's RestoreSandboxData against throwaway folders.
// Build (the .NET Framework compiler that ships with Windows):
//   csc /main:UninstallSmoke /out:<tmp>\uninstall_smoke.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll
//       installer\Installer.cs tests\uninstall_smoke.cs
// Run: uninstall_smoke <empty scratch folder>
using System;
using System.IO;
using MeowyInstaller;

static class UninstallSmoke
{
	static int failed;

	static void Check(bool ok, string what)
	{
		Console.WriteLine((ok ? "ok   " : "FAIL ") + what);
		if (!ok)
			failed++;
	}

	static void Put(string path, string text)
	{
		Directory.CreateDirectory(Path.GetDirectoryName(path));
		File.WriteAllText(path, text);
	}

	static string Text(string path)
	{
		return File.Exists(path) ? File.ReadAllText(path) : null;
	}

	static int Main(string[] args)
	{
		if (args.Length < 1)
		{
			Console.WriteLine("usage: uninstall_smoke <empty scratch folder>");
			return 2;
		}
		string root = Path.GetFullPath(args[0]);

		// 1. At the menu (the usual case): two vaults, a clash, an identical file.
		{
			string s = Path.Combine(root, "menu");
			string data = Path.Combine(s, "data"), local = Path.Combine(s, "local"), steam = Path.Combine(s, "steam"), backup = Path.Combine(s, "backup");
			Put(Path.Combine(data, "menu_now.txt"), "menu");
			Put(Path.Combine(data, "shared.txt"), "menu version");
			Put(Path.Combine(data, "same.txt"), "same");
			Put(Path.Combine(local, "active"), "menu");
			Put(Path.Combine(local, @"before-sandbox\old.txt"), "old");
			Put(Path.Combine(local, @"before-sandbox\shared.txt"), "old version");
			Put(Path.Combine(local, @"before-sandbox\same.txt"), "same");
			Put(Path.Combine(local, @"before-sandbox\adv2\dupe.txt"), "dupe");
			Put(Path.Combine(local, @"1.2.3.4_27015\a.txt"), "a");
			Put(Path.Combine(local, @"1.2.3.4_27015\nested\x.txt"), "x");
			Put(Path.Combine(steam, "active"), "menu");
			Put(Path.Combine(steam, @"before-sandbox\steamold.txt"), "steam old");
			Put(Path.Combine(steam, @"5.6.7.8_27015\b.txt"), "b");

			Program.RestoreReport r = Program.RestoreSandboxData(data, local, steam, true, backup);
			Console.WriteLine(Program.Describe(r));
			Check(Text(Path.Combine(data, "menu_now.txt")) == "menu", "menu: the menu's file stays");
			Check(Text(Path.Combine(data, "shared.txt")) == "menu version", "menu: a file in data/ is never overwritten");
			Check(Text(Path.Combine(data, "shared.before-pack.txt")) == "old version", "menu: on a clash both kept, the old one suffixed");
			Check(Text(Path.Combine(data, "old.txt")) == "old" && Text(Path.Combine(data, @"adv2\dupe.txt")) == "dupe", "menu: before-sandbox back in data/");
			Check(Text(Path.Combine(data, "same.txt")) == "same" && !File.Exists(Path.Combine(data, "same.before-pack.txt")), "menu: an identical file isn't doubled");
			Check(Text(Path.Combine(data, "steamold.txt")) == "steam old", "menu: the steamapps vault's before-sandbox back too");
			Check(Text(Path.Combine(backup, @"1.2.3.4_27015\a.txt")) == "a" && Text(Path.Combine(backup, @"1.2.3.4_27015\nested\x.txt")) == "x",
				"menu: a server's files in the backup");
			Check(Text(Path.Combine(backup, @"5.6.7.8_27015\b.txt")) == "b", "menu: the steamapps vault's servers in the backup");
			Check(!File.Exists(Path.Combine(data, "a.txt")) && !File.Exists(Path.Combine(data, "b.txt")), "menu: no server's file in data/");
			Check(File.Exists(Path.Combine(backup, "README.txt")), "menu: the backup says what it is");
			Check(!Directory.Exists(local) && !Directory.Exists(steam), "menu: both vaults emptied and removed");
			Check(r.Left.Count == 0 && r.KeptVaults.Count == 0, "menu: nothing left behind");
		}

		// 2. The game went down while on a server: data/ holds that server's files.
		{
			string s = Path.Combine(root, "crashed");
			string data = Path.Combine(s, "data"), local = Path.Combine(s, "local"), backup = Path.Combine(s, "backup");
			Put(Path.Combine(data, "c_now.txt"), "c now");
			Put(Path.Combine(local, "active"), "9.9.9.9_27015");
			Put(Path.Combine(local, @"menu\m.txt"), "m");
			Put(Path.Combine(local, @"9.9.9.9_27015\c_old.txt"), "c old");
			Program.RestoreSandboxData(data, local, Path.Combine(s, "nosteam"), true, backup);
			Check(Text(Path.Combine(data, "m.txt")) == "m" && !File.Exists(Path.Combine(data, "c_now.txt")), "crashed: the menu's files back, the server's out");
			Check(Text(Path.Combine(backup, @"9.9.9.9_27015\c_now.txt")) == "c now" && Text(Path.Combine(backup, @"9.9.9.9_27015\c_old.txt")) == "c old",
				"crashed: all of that server's files together in the backup");
			Check(!Directory.Exists(local), "crashed: vault removed");
		}

		// 3. Mid-switch, phase 2 (moving a server's files in): data/ is that server's.
		{
			string s = Path.Combine(root, "switching");
			string data = Path.Combine(s, "data"), local = Path.Combine(s, "local"), backup = Path.Combine(s, "backup");
			Put(Path.Combine(data, "s1.txt"), "s1");
			Put(Path.Combine(local, "active"), "switching:menu>1.1.1.1_27015:2");
			Put(Path.Combine(local, @"menu\m.txt"), "m");
			Put(Path.Combine(local, @"1.1.1.1_27015\s2.txt"), "s2");
			Program.RestoreSandboxData(data, local, null, true, backup);
			Check(Text(Path.Combine(data, "m.txt")) == "m" && !File.Exists(Path.Combine(data, "s1.txt")), "switching 2: the menu's back, the server's out");
			Check(File.Exists(Path.Combine(backup, @"1.1.1.1_27015\s1.txt")) && File.Exists(Path.Combine(backup, @"1.1.1.1_27015\s2.txt")),
				"switching 2: the server's files together");
		}

		// 4. Mid-switch, phase 1 (moving the menu's files out): data/ is the menu's.
		{
			string s = Path.Combine(root, "switching1");
			string data = Path.Combine(s, "data"), local = Path.Combine(s, "local"), backup = Path.Combine(s, "backup");
			Put(Path.Combine(data, "m1.txt"), "m1");
			Put(Path.Combine(local, "active"), "switching:menu>1.1.1.1_27015:1");
			Put(Path.Combine(local, @"menu\m2.txt"), "m2");
			Put(Path.Combine(local, @"1.1.1.1_27015\s.txt"), "s");
			Program.RestoreSandboxData(data, local, null, true, backup);
			Check(File.Exists(Path.Combine(data, "m1.txt")) && File.Exists(Path.Combine(data, "m2.txt")), "switching 1: all the menu's files in data/");
			Check(File.Exists(Path.Combine(backup, @"1.1.1.1_27015\s.txt")) && !File.Exists(Path.Combine(data, "s.txt")), "switching 1: the server's in the backup");
		}

		// 5. A backup from an earlier uninstall, and a file that can't be moved.
		{
			string s = Path.Combine(root, "again");
			string data = Path.Combine(s, "data"), local = Path.Combine(s, "local"), backup = Path.Combine(s, "backup");
			Put(Path.Combine(backup, @"1.2.3.4_27015\a.txt"), "a from before");
			Put(Path.Combine(local, "active"), "menu");
			Put(Path.Combine(local, @"1.2.3.4_27015\a.txt"), "a now");
			Put(Path.Combine(local, @"before-sandbox\locked.txt"), "locked");
			Program.RestoreReport r;
			using (new FileStream(Path.Combine(local, @"before-sandbox\locked.txt"), FileMode.Open, FileAccess.Read, FileShare.None))
				r = Program.RestoreSandboxData(data, local, null, true, backup);
			Check(Text(Path.Combine(backup, @"1.2.3.4_27015\a.txt")) == "a from before" && Text(Path.Combine(backup, @"1.2.3.4_27015\a.older.txt")) == "a now",
				"again: an earlier backup kept, the new copy beside it");
			Check(r.Left.Count == 1 && File.Exists(Path.Combine(local, @"before-sandbox\locked.txt")), "again: a file in use stays where it is");
			Check(Directory.Exists(local) && r.KeptVaults.Count == 1, "again: a vault with something left in it is never deleted");
			Check(!File.Exists(Path.Combine(local, "active")), "again: the marker goes (data/ is the player's again)");
		}

		// 6. No vault at all (installed, never started).
		{
			string s = Path.Combine(root, "none");
			Program.RestoreReport r = Program.RestoreSandboxData(Path.Combine(s, "data"), Path.Combine(s, "local"), Path.Combine(s, "steam"), true, Path.Combine(s, "backup"));
			Check(r.Vaults.Count == 0 && !Directory.Exists(Path.Combine(s, "backup")), "none: nothing to do, no backup folder made");
		}

		Console.WriteLine(failed > 0 ? "FAILED" : "all passed");
		return failed > 0 ? 1 : 0;
	}
}
