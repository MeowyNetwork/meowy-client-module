// uninstall_smoke: the installer's RestoreSandboxData against throwaway folders,
// and its Start-menu shortcut for notifications (in a scratch folder).
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

	static bool IsLink(string path)
	{
		return Directory.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0;
	}

	// A directory junction, as the pack makes them.
	static void Link(string link, string target)
	{
		Directory.CreateDirectory(Path.GetDirectoryName(link));
		var p = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe",
			"/c mklink /J \"" + link + "\" \"" + target + "\"") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true });
		p.WaitForExit();
	}

	[STAThread]
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

		// 7. 1.6.1+, at the menu: data\ is a link to the vault's menu folder.
		{
			string s = Path.Combine(root, "linked");
			string data = Path.Combine(s, "data"), local = Path.Combine(s, "local"), backup = Path.Combine(s, "backup");
			Put(Path.Combine(local, @"menu\m.txt"), "m");
			Put(Path.Combine(local, @"menu\sub\deep.txt"), "deep");
			Put(Path.Combine(local, @"before-sandbox\old.txt"), "old");
			Put(Path.Combine(local, @"1.2.3.4_27015\a.txt"), "a");
			Put(Path.Combine(local, @"menu.incoming\staged.txt"), "staged");          // a start that died converting
			Put(Path.Combine(local, @"before-sandbox.incoming-2\staged2.txt"), "staged2");
			Put(Path.Combine(local, "active"), "linked");
			Link(data, Path.Combine(local, "menu"));
			Link(data + ".meowy-link", Path.Combine(local, "menu"));   // left by a start that died while linking
			Program.RestoreReport r = Program.RestoreSandboxData(data, local, null, true, backup);
			Console.WriteLine(Program.Describe(r));
			Check(!IsLink(data) && Text(Path.Combine(data, "m.txt")) == "m" && Text(Path.Combine(data, @"sub\deep.txt")) == "deep",
				"linked: data\\ a plain folder again, with the menu's files");
			Check(Text(Path.Combine(data, "old.txt")) == "old", "linked: before-sandbox back too");
			Check(Text(Path.Combine(data, "staged.txt")) == "staged" && Text(Path.Combine(data, "staged2.txt")) == "staged2" &&
				!Directory.Exists(Path.Combine(backup, "menu.incoming")), "linked: leftover staging folders of the player's own back in data\\");
			Check(Text(Path.Combine(backup, @"1.2.3.4_27015\a.txt")) == "a" && !File.Exists(Path.Combine(data, "a.txt")), "linked: the server's in the backup");
			Check(!Directory.Exists(data + ".meowy-link") && !Directory.Exists(local) && r.Left.Count == 0, "linked: leftover link and vault gone, nothing left");
		}

		// 8. 1.6.1+, the game went down on a server: data\ links to that server's folder.
		{
			string s = Path.Combine(root, "linkedcrash");
			string data = Path.Combine(s, "data"), local = Path.Combine(s, "local"), backup = Path.Combine(s, "backup");
			Put(Path.Combine(local, @"menu\m.txt"), "m");
			Put(Path.Combine(local, @"9.9.9.9_27015\c.txt"), "c");
			Put(Path.Combine(local, "active"), "linked");
			Link(data, Path.Combine(local, "9.9.9.9_27015"));
			Program.RestoreSandboxData(data, local, null, true, backup);
			Check(!IsLink(data) && Text(Path.Combine(data, "m.txt")) == "m" && !File.Exists(Path.Combine(data, "c.txt")),
				"linked crash: the menu's files in data\\, not the server's");
			Check(Text(Path.Combine(backup, @"9.9.9.9_27015\c.txt")) == "c", "linked crash: the server's files in the backup");
		}

		// 9. The player's own link (data\ kept on another drive): never removed.
		{
			string s = Path.Combine(root, "ownlink");
			string data = Path.Combine(s, "data"), local = Path.Combine(s, "local"), backup = Path.Combine(s, "backup");
			string elsewhere = Path.Combine(s, "elsewhere");
			Put(Path.Combine(elsewhere, "mine.txt"), "mine");
			Put(Path.Combine(local, @"before-sandbox\old.txt"), "old");
			Link(data, elsewhere);
			Program.RestoreSandboxData(data, local, null, true, backup);
			Check(IsLink(data) && Text(Path.Combine(elsewhere, "mine.txt")) == "mine" && Text(Path.Combine(elsewhere, "old.txt")) == "old",
				"own link: kept, and the old files put back through it");
		}

		// 10. The Start-menu shortcut for notifications, in a scratch "Start menu"
		//    (never the real one): made with the AUMID, repaired, removed.
		{
			string s = Path.Combine(root, "shortcut");
			string garrysmod = Path.Combine(s, "garrysmod");
			Put(Path.Combine(garrysmod, @"lua\bin\gmcl_meowy_win64.dll"), "not really a dll");
			string lnk = Path.Combine(s, @"Start Menu\Programs\" + Program.ShortcutFileName);
			Check(Program.ShortcutFileName == "Meowy Client Pack.lnk", "shortcut: named \"Meowy Client Pack\" (the name the module looks for)");
			Check(Program.ShortcutAppId(lnk) == null, "shortcut: none before install");

			Program.CreateShortcut(lnk, garrysmod);
			Check(File.Exists(lnk), "shortcut: install makes it (and its folder)");
			Check(Program.ShortcutAppId(lnk) == "MeowyNetwork.ClientPack", "shortcut: carries System.AppUserModel.ID = MeowyNetwork.ClientPack");
			string target = Program.ShortcutTarget(lnk);
			Check(target.EndsWith("explorer.exe steam://rungameid/4000", StringComparison.OrdinalIgnoreCase),
				"shortcut: starts Garry's Mod through Steam (" + target + ")");

			// Reinstall over a damaged one: a file that isn't a shortcut at all.
			File.WriteAllText(lnk, "damaged");
			Check(Program.ShortcutAppId(lnk) == null, "shortcut: a damaged one reads as missing");
			Program.CreateShortcut(lnk, garrysmod);
			Check(Program.ShortcutAppId(lnk) == "MeowyNetwork.ClientPack", "shortcut: reinstall repairs it");
			Program.CreateShortcut(lnk, garrysmod);
			Check(Program.ShortcutAppId(lnk) == "MeowyNetwork.ClientPack" && Directory.GetFiles(Path.GetDirectoryName(lnk)).Length == 1,
				"shortcut: installing again leaves exactly one");

			Program.RemoveShortcut(lnk);
			Check(!File.Exists(lnk), "shortcut: uninstall removes it");
			Program.RemoveShortcut(lnk);
			Check(!File.Exists(lnk), "shortcut: uninstall without one is fine");
			Check(Program.StartMenuShortcut().EndsWith(@"\Programs\Meowy Client Pack.lnk", StringComparison.OrdinalIgnoreCase),
				"shortcut: the real one goes in the player's own Start menu (" + Program.StartMenuShortcut() + ")");

			// With the Meowy Launcher installed, the shortcut opens it, same AUMID.
			string launcher = Path.Combine(s, @"launcher\MeowyLauncher.exe");
			Put(launcher, "not really an exe");
			Program.CreateShortcut(lnk, garrysmod, launcher);
			Check(Program.ShortcutAppId(lnk) == "MeowyNetwork.ClientPack", "shortcut: with the launcher, same AUMID");
			target = Program.ShortcutTarget(lnk).Trim();
			Check(string.Equals(target, launcher, StringComparison.OrdinalIgnoreCase), "shortcut: opens the launcher (" + target + ")");
			Program.RemoveShortcut(lnk);
		}

		// 11. The launcher's registry values, under scratch keys (never the real
		//     meowy:// or Run key).
		{
			string key = @"Software\MeowyClientPackTest\Classes\meowy";
			string run = @"Software\MeowyClientPackTest\Run";
			string exe = Path.Combine(root, @"launcher\MeowyLauncher.exe");
			string other = Path.Combine(root, @"someone-else\app.exe");
			try
			{
				Program.RegisterProtocol(exe, key);
				Check(Program.ProtocolExe(key) == exe, "meowy://: opens the launcher");
				using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(key))
					Check(k != null && k.GetValue("URL Protocol") as string == "", "meowy://: marked as a URL protocol");
				using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(key + @"\shell\open\command"))
					Check(k.GetValue("") as string == "\"" + exe + "\" \"%1\"", "meowy://: passes the link as one argument");
				Program.UnregisterProtocol(other, key);
				Check(Program.ProtocolExe(key) == exe, "meowy://: uninstall leaves it when it isn't ours");
				Program.UnregisterProtocol(exe, key);
				Check(Program.ProtocolExe(key) == null, "meowy://: uninstall removes ours");

				Program.SetLauncherAtSignIn(exe, true, run);
				Check(Program.LauncherAtSignIn(run), "sign-in: Run value set");
				using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(run))
					Check(k.GetValue(Program.LauncherRunValue) as string == "\"" + exe + "\" --tray", "sign-in: starts in the tray");
				Program.SetLauncherAtSignIn(exe, false, run);
				Check(!Program.LauncherAtSignIn(run), "sign-in: Run value removed");
				Program.SetLauncherAtSignIn(exe, false, run);
				Check(!Program.LauncherAtSignIn(run), "sign-in: removing twice is fine");
			}
			finally
			{
				Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(@"Software\MeowyClientPackTest", false);
			}
		}

		// 12. "Start with the game" as the player chose it, not as Steam left menu.lua.
		{
			Check(Program.StartupWanted(true, false), "start-up: the line there");
			Check(Program.StartupWanted(false, false), "start-up: the line gone, never turned off: ticked (and put back)");
			Check(!Program.StartupWanted(false, true), "start-up: turned off by the player: unticked");
			Check(Program.StartupWanted(true, true), "start-up: the line there wins");
		}

		// 13. Reinstalling: downloaded versions, their pointers and the seq floors go.
		{
			string d = Path.Combine(root, "reset");
			foreach (string f in new[] { @"state\current_win64.txt", @"state\pending_win32.txt", @"state\release_seq.txt",
				@"state\release_seq_win64.txt", @"state\release_seq_win32.txt", @"state\bad_win64.txt", @"state\policy.txt", @"bin\1.7.1\gmcl_meowy_win64.dll" })
				Put(Path.Combine(d, f), "x");
			Program.ResetUpdates(d);
			Check(!File.Exists(Path.Combine(d, @"state\release_seq.txt")) && !File.Exists(Path.Combine(d, @"state\release_seq_win64.txt"))
				&& !File.Exists(Path.Combine(d, @"state\release_seq_win32.txt")), "reset: the seq floors go");
			Check(!File.Exists(Path.Combine(d, @"state\current_win64.txt")) && !File.Exists(Path.Combine(d, @"state\pending_win32.txt"))
				&& !Directory.Exists(Path.Combine(d, "bin")), "reset: pointers and downloads go");
			Check(File.Exists(Path.Combine(d, @"state\bad_win64.txt")) && File.Exists(Path.Combine(d, @"state\policy.txt")), "reset: the bad list and the policy stay");
			// --reset-policy: the kept policy and the stub's copy go, nothing else.
			Put(Path.Combine(d, @"state\policy.sig"), "x");
			Put(Path.Combine(d, @"state\policy.bin"), "enabled 0\nseq 9999999999999999999\n");
			Check(Program.ResetPolicy(d).Contains("forgotten") && !File.Exists(Path.Combine(d, @"state\policy.txt"))
				&& !File.Exists(Path.Combine(d, @"state\policy.sig")) && !File.Exists(Path.Combine(d, @"state\policy.bin"))
				&& File.Exists(Path.Combine(d, @"state\bad_win64.txt")), "reset policy: the kept policy goes, the rest stays");
			Check(Program.ResetPolicy(d) == "policy: none kept", "reset policy: again, nothing to do");
		}

		// 14. Uninstall without Garry's Mod: the pack's own files go, the vault stays.
		{
			string d = Path.Combine(root, "own");
			Put(Path.Combine(d, "menu_settings.txt"), "x");
			Put(Path.Combine(d, @"launcher\MeowyLauncher.exe"), "x");
			Put(Path.Combine(d, @"vault\menu\save.txt"), "mine");
			Program.RemoveOwnData(d);
			Check(Text(Path.Combine(d, @"vault\menu\save.txt")) == "mine", "own files: the vault is kept");
			Check(!File.Exists(Path.Combine(d, "menu_settings.txt")) && !Directory.Exists(Path.Combine(d, "launcher")), "own files: the rest goes");
			string e = Path.Combine(root, "own-empty");
			Put(Path.Combine(e, "x.txt"), "x");
			Program.RemoveOwnData(e);
			Check(!Directory.Exists(e), "own files: nothing left, the folder goes too");
		}

		// 15. The launcher's lazy fonts put back (the game's folder, as fonts.rs restore).
		{
			string gm = Path.Combine(root, @"fonts\garrysmod");
			Put(Path.Combine(gm, Program.LazyFontsAside + @"\a.ttf"), "a");
			Put(Path.Combine(gm, Program.LazyFontsAside + @"\b.ttf"), "b");
			Check(Program.RestoreLazyFonts(gm) == 2 && Text(Path.Combine(gm, Program.LazyFonts + @"\a.ttf")) == "a"
				&& !Directory.Exists(Path.Combine(gm, Program.LazyFontsAside)), "fonts: the folder back");
			Check(Program.RestoreLazyFonts(gm) == 0, "fonts: nothing aside, nothing done");
			// The game made a new fonts folder meanwhile: merged, its copies winning.
			Put(Path.Combine(gm, Program.LazyFontsAside + @"\a.ttf"), "old a");
			Put(Path.Combine(gm, Program.LazyFontsAside + @"\c.ttf"), "c");
			Check(Program.RestoreLazyFonts(gm) == 1 && Text(Path.Combine(gm, Program.LazyFonts + @"\a.ttf")) == "a"
				&& Text(Path.Combine(gm, Program.LazyFonts + @"\c.ttf")) == "c" && !Directory.Exists(Path.Combine(gm, Program.LazyFontsAside)),
				"fonts: merged into a new folder, the game's copy kept");
		}

		Console.WriteLine(failed > 0 ? "FAILED" : "all passed");
		return failed > 0 ? 1 : 0;
	}
}
