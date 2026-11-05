// Meowy Client Pack installer.
//
// Finds Garry's Mod through Steam's own files, then copies the embedded
// modules into garrysmod/lua/bin: gmcl_meowy_win32/win64.dll (both, so it
// works on either Steam branch) and the start-up stubs gmsv_meowy_boot_win32/
// win64.dll. With "Start with the game" on (the default) it also adds one
// marked line to garrysmod/lua/includes/menu.lua that loads the stub when the
// game starts. Uninstall first puts back the garrysmod/data files the pack kept
// apart (the player's own into garrysmod/data, each server's into a backup
// folder in Documents; see RestoreSandboxData), then removes exactly those
// files, that line and the pack's data folder (%LOCALAPPDATA%\MeowyClientPack,
// and garrysmod/data/meowy from before 1.4.1). It needs no admin rights when the Steam folder is writable,
// which is Steam's default. Install also makes a "Meowy Client Pack" shortcut in
// the player's Start menu carrying the AppUserModelID the module's Windows
// notifications use (Windows needs one; see CreateShortcut); uninstall removes it.
//
// Built with the .NET Framework 4.x compiler that ships with Windows
// (C# 5), so there is nothing to install to build or run it. See build.ps1.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("Meowy Client Pack")]
[assembly: AssemblyProduct("Meowy Client Pack")]
[assembly: AssemblyVersion("1.5.0.0")]

namespace MeowyInstaller
{
	static class Program
	{
		public const string ServerAddress = "138.201.62.52:27015";
		public const string SourceUrl = "https://github.com/MeowyNetwork/meowy-client-module";
		public static readonly string[] Modules = {
			"gmcl_meowy_win32.dll", "gmcl_meowy_win64.dll", "gmsv_meowy_boot_win32.dll", "gmsv_meowy_boot_win64.dll" };

		// The start-up line. Matched by its marker, so later versions can rewrite it.
		const string HookMarker = "meowy-client-pack";
		const string HookLine = "pcall(require, \"meowy_boot\") -- meowy-client-pack v1 " +
			"(loads the Meowy Client Pack at start-up; delete this line to disable)";
		static readonly string[] HookHosts = { @"lua\includes\menu.lua", @"lua\menu\menu.lua" };

		// No arguments: the window. "--install" / "--uninstall" [path] [--no-startup]:
		// no window; exit code 0 on success, 1 on failure, details on stdout.
		[STAThread]
		static int Main(string[] args)
		{
			if (args.Length > 0 && (args[0] == "--install" || args[0] == "--uninstall"))
			{
				bool startup = Array.IndexOf(args, "--no-startup") < 0;
				string path = args.Length > 1 && !args[1].StartsWith("--") ? args[1] : FindGarrysMod();
				return RunCommandLine(args[0] == "--install", path, startup);
			}

			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);
			Application.Run(new MainForm());
			return 0;
		}

		// A winexe has no console of its own; print to the one that started it.
		[System.Runtime.InteropServices.DllImport("kernel32.dll")]
		static extern bool AttachConsole(int processId);

		static int RunCommandLine(bool install, string garrysmod, bool startup)
		{
			AttachConsole(-1);
			if (string.IsNullOrEmpty(garrysmod) || !Directory.Exists(Path.Combine(garrysmod, "lua")))
			{
				Console.WriteLine("Garry's Mod not found. Pass the garrysmod folder as the second argument.");
				return 1;
			}
			if (IsGameRunning())
			{
				Console.WriteLine("Close Garry's Mod first.");
				return 1;
			}
			try
			{
				// Uninstall: the player's files first, while the pack is still whole.
				if (!install)
					Console.WriteLine(Describe(RemoveData(garrysmod)));
				string bin = Path.Combine(garrysmod, @"lua\bin");
				Directory.CreateDirectory(bin);
				foreach (string module in Modules)
				{
					string file = Path.Combine(bin, module);
					if (install)
						ExtractTo(module, file);
					else if (File.Exists(file))
						File.Delete(file);
					Console.WriteLine((install ? "installed " : "removed ") + file);
				}
				if (install && startup)
					AddStartupHook(garrysmod);
				else
					RemoveStartupHook(garrysmod);
				Console.WriteLine("start with the game: " + (StartupHookPresent(garrysmod) ? "on" : "off"));
				Console.WriteLine(install ? InstallShortcut(garrysmod) : UninstallShortcut());
				return 0;
			}
			catch (Exception e)
			{
				Console.WriteLine("failed: " + e.Message);
				return 1;
			}
		}

		// Garry's Mod's garrysmod folder, or null if Steam doesn't know it.
		public static string FindGarrysMod()
		{
			string steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string;
			if (string.IsNullOrEmpty(steam))
				return null;

			var libraries = new List<string>();
			libraries.Add(steam);
			string vdf = Path.Combine(steam, @"steamapps\libraryfolders.vdf");
			if (File.Exists(vdf))
			{
				foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
					libraries.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
			}

			foreach (string lib in libraries)
			{
				if (!File.Exists(Path.Combine(lib, @"steamapps\appmanifest_4000.acf")))
					continue;
				string dir = Path.Combine(lib, @"steamapps\common\GarrysMod\garrysmod");
				if (Directory.Exists(dir))
					return Path.GetFullPath(dir);
			}
			return null;
		}

		public static bool IsGameRunning()
		{
			return Process.GetProcessesByName("gmod").Length > 0 || Process.GetProcessesByName("hl2").Length > 0;
		}

		// --- Start-up line ------------------------------------------------------

		public static bool StartupHookPresent(string garrysmod)
		{
			foreach (string host in HookHosts)
			{
				string file = Path.Combine(garrysmod, host);
				if (File.Exists(file) && File.ReadAllText(file).Contains(HookMarker))
					return true;
			}
			return false;
		}

		// Appends the line to lua/includes/menu.lua (CRLF, like the file), unless
		// a line with the marker is already in either host file.
		public static void AddStartupHook(string garrysmod)
		{
			if (StartupHookPresent(garrysmod))
				return;
			string file = Path.Combine(garrysmod, HookHosts[0]);
			if (!File.Exists(file))
				throw new IOException(@"lua\includes\menu.lua not found; verify Garry's Mod's files in Steam");
			string text = File.ReadAllText(file);
			if (text.Length > 0 && !text.EndsWith("\n"))
				text += "\r\n";
			WriteReplacing(file, text + HookLine + "\r\n");
		}

		// Removes every line with the marker from both host files, and the line
		// break AddStartupHook put in front of it.
		public static void RemoveStartupHook(string garrysmod)
		{
			foreach (string host in HookHosts)
			{
				string file = Path.Combine(garrysmod, host);
				if (!File.Exists(file))
					continue;
				string text = File.ReadAllText(file);
				if (!text.Contains(HookMarker))
					continue;
				var kept = new List<string>();
				foreach (string line in text.Split('\n'))
					if (!line.Contains(HookMarker))
						kept.Add(line);
				string result = string.Join("\n", kept.ToArray());
				if (result.EndsWith("\r\n"))
					result = result.Substring(0, result.Length - 2);
				WriteReplacing(file, result);
			}
		}

		// Write to a temporary file next to the target, then swap it in.
		static void WriteReplacing(string file, string text)
		{
			var attrs = File.GetAttributes(file);
			if ((attrs & FileAttributes.ReadOnly) != 0)
				File.SetAttributes(file, attrs & ~FileAttributes.ReadOnly);
			string temp = file + ".meowy-tmp";
			File.WriteAllText(temp, text);
			File.Replace(temp, file, null);
		}

		// The pack's files: %LOCALAPPDATA%\MeowyClientPack (boot.lock, join times,
		// WebView2's profile), and garrysmod\data\meowy where they were before 1.4.1.
		// The player's own files the pack kept apart are put back first
		// (RestoreSandboxData); a vault that couldn't be emptied is never deleted.
		public static RestoreReport RemoveData(string garrysmod)
		{
			RestoreReport report = RestoreSandboxData(garrysmod);
			string old = Path.Combine(garrysmod, @"data\meowy");
			if (Directory.Exists(old))
				Directory.Delete(old, true);
			string local = Environment.GetEnvironmentVariable("LOCALAPPDATA");
			if (!string.IsNullOrEmpty(local))
			{
				string dir = Path.Combine(local, "MeowyClientPack");
				if (Directory.Exists(dir))
				{
					foreach (string sub in Directory.GetDirectories(dir))
						if (!string.Equals(Path.GetFileName(sub), "vault", StringComparison.OrdinalIgnoreCase))
							Directory.Delete(sub, true);
					foreach (string file in Directory.GetFiles(dir))
						File.Delete(file);
					if (Directory.GetFileSystemEntries(dir).Length == 0)
						Directory.Delete(dir, false);
				}
			}
			return report;
		}

		// --- The player's files the pack kept apart (src/data_sandbox.cpp) ------
		//
		// With the pack, garrysmod\data holds only the files of the server being
		// played, or the main menu's; the rest wait in a vault:
		// %LOCALAPPDATA%\MeowyClientPack\vault, or steamapps\MeowyClientPackVault
		// when the game is on another drive. In it: "before-sandbox" (all of data\
		// from before the pack), "menu" (the main menu's files while on a server),
		// a folder per server, and "active" (whose files data\ holds right now).
		//
		// Uninstall puts the player's own files (before-sandbox and menu) back in
		// garrysmod\data, never overwriting anything there: on a name clash both
		// are kept, the one coming back under a suffixed name. Each server's own
		// files go to a backup folder in Documents, not into data\: any server's
		// Lua can read all of data\ (and BASE_PATH, the game folder), and those
		// files are what one server wrote, sometimes about where else you play.

		public class RestoreReport
		{
			public int ToData, Renamed, ToBackup, Duplicates;
			public string BackupDir;
			public readonly List<string> Vaults = new List<string>();      // vaults found
			public readonly List<string> KeptVaults = new List<string>();  // vaults that couldn't be emptied
			public readonly List<string> Left = new List<string>();        // what couldn't be moved
		}

		[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
		static extern bool GetVolumePathNameW(string fileName, System.Text.StringBuilder volumePathName, int bufferLength);

		static string VolumeOf(string path)
		{
			var sb = new System.Text.StringBuilder(1024);
			return GetVolumePathNameW(path, sb, sb.Capacity) ? sb.ToString().ToLowerInvariant() : null;
		}

		public static string BackupDirFor(string garrysmod)
		{
			string docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
			// No Documents folder: beside the game, outside its folder (steamapps).
			if (string.IsNullOrEmpty(docs))
				docs = Path.GetFullPath(Path.Combine(garrysmod, @"..\..\.."));
			return Path.Combine(docs, "Meowy Client Pack backup");
		}

		// Both vault locations, the one the pack uses first (data_sandbox.cpp VaultDir).
		public static RestoreReport RestoreSandboxData(string garrysmod)
		{
			string data = Path.Combine(garrysmod, "data");
			string localBase = Environment.GetEnvironmentVariable("LOCALAPPDATA");
			string local = string.IsNullOrEmpty(localBase) ? null : Path.Combine(localBase, @"MeowyClientPack\vault");
			// garrysmod -> GarrysMod -> common -> steamapps
			string steam = Path.GetFullPath(Path.Combine(garrysmod, @"..\..\..\MeowyClientPackVault"));
			string dataVolume = VolumeOf(data);
			bool localCurrent = local != null && dataVolume != null && VolumeOf(localBase) == dataVolume;
			return RestoreSandboxData(data, local, steam, localCurrent, BackupDirFor(garrysmod));
		}

		// The same with every folder given (for tests).
		public static RestoreReport RestoreSandboxData(string data, string localVault, string steamVault, bool localIsCurrent, string backup)
		{
			var r = new RestoreReport();
			r.BackupDir = backup;
			Directory.CreateDirectory(data);
			string current = localIsCurrent ? localVault : steamVault;
			string other = localIsCurrent ? steamVault : localVault;
			if (current != null)
				RestoreVault(data, current, true, backup, r);
			if (other != null && !string.Equals(other, current, StringComparison.OrdinalIgnoreCase))
				RestoreVault(data, other, false, backup, r);
			return r;
		}

		// Whose files data\ holds, from the vault's "active" file (as Recover()
		// reads it): "switching:<from>><to>:<phase>" mid-switch is <from>'s in
		// phase 1 (moving out) and <to>'s in phase 2 (moving in).
		static string OwnerOf(string active)
		{
			if (!active.StartsWith("switching:", StringComparison.Ordinal))
				return active;
			string rest = active.Substring(10);
			int arrow = rest.IndexOf('>'), colon = rest.LastIndexOf(':');
			if (arrow >= 0 && colon > arrow)
				return rest.Substring(colon + 1) == "1" ? rest.Substring(0, arrow) : rest.Substring(arrow + 1, colon - arrow - 1);
			return rest; // the old marker, "switching:<to>"
		}

		static bool IsPlayersOwn(string key)
		{
			return key == "" || key == "menu" || key == "before-sandbox";
		}

		// A key as a single folder name: never a path, never only dots.
		static string SafeName(string key)
		{
			if (key.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || key.Trim('.', ' ') == "")
				return "unknown";
			return key;
		}

		static void RestoreVault(string data, string vault, bool current, string backup, RestoreReport r)
		{
			if (!Directory.Exists(vault))
				return;
			r.Vaults.Add(vault);
			string activeFile = Path.Combine(vault, "active");
			string owner = File.Exists(activeFile) ? OwnerOf(File.ReadAllText(activeFile).Trim()) : "";
			// The game went down while on a server: data\ holds that server's
			// files. They go with the rest of that server's, not to the player.
			if (current && !IsPlayersOwn(owner))
				MergeMove(data, Path.Combine(backup, SafeName(owner)), "at-uninstall", false, r);

			// The player's own: the main menu's (the newest), then everything from before the pack.
			string menu = Path.Combine(vault, "menu"), before = Path.Combine(vault, "before-sandbox");
			if (Directory.Exists(menu))
				MergeMove(menu, data, "menu", true, r);
			if (Directory.Exists(before))
				MergeMove(before, data, "before-pack", true, r);

			// Each server's own files.
			foreach (string dir in Directory.GetDirectories(vault))
			{
				string name = Path.GetFileName(dir);
				if (name == "menu" || name == "before-sandbox")
					continue;
				MergeMove(dir, Path.Combine(backup, SafeName(name)), "older", false, r);
			}

			// data\ now holds everything that's the player's, as before the pack:
			// the marker goes, and the vault too once nothing is left in it. A
			// vault something is left in is never deleted.
			if (File.Exists(activeFile))
				File.Delete(activeFile);
			RemoveEmpty(vault);
			if (Directory.Exists(vault))
				r.KeptVaults.Add(vault);
			if (r.ToBackup > 0)
				WriteBackupNote(backup);
		}

		// Moves everything in `src` into `dst` (created if missing), file by file
		// (File.Move also works across drives). Nothing in `dst` is overwritten:
		// an identical file there makes the moved one redundant, a different one
		// keeps its name and the moved one gets "<name>.<label>[-N].<ext>".
		static void MergeMove(string src, string dst, string label, bool toData, RestoreReport r)
		{
			string[] dirs, files;
			try
			{
				Directory.CreateDirectory(dst);
				dirs = Directory.GetDirectories(src);
				files = Directory.GetFiles(src);
			}
			catch (Exception)
			{
				r.Left.Add(src);
				return;
			}
			foreach (string file in files)
			{
				string name = Path.GetFileName(file);
				string target = Path.Combine(dst, name);
				try
				{
					if (File.Exists(target) || Directory.Exists(target))
					{
						if (File.Exists(target) && SameContent(file, target))
						{
							File.Delete(file);
							r.Duplicates++;
							continue;
						}
						target = FreeName(dst, name, label);
						if (toData)
							r.Renamed++;
					}
					File.Move(file, target);
					if (toData) r.ToData++; else r.ToBackup++;
				}
				catch (Exception)
				{
					r.Left.Add(file);
				}
			}
			foreach (string dir in dirs)
			{
				// The pack never makes links, and servers can't: leave any alone.
				if ((File.GetAttributes(dir) & FileAttributes.ReparsePoint) != 0)
				{
					r.Left.Add(dir);
					continue;
				}
				string name = Path.GetFileName(dir);
				string target = Path.Combine(dst, name);
				if (File.Exists(target))
					target = FreeName(dst, name, label);
				MergeMove(dir, target, label, toData, r);
				RemoveEmpty(dir);
			}
		}

		static string FreeName(string dir, string name, string label)
		{
			string stem = Path.GetFileNameWithoutExtension(name), ext = Path.GetExtension(name);
			for (int i = 1; ; i++)
			{
				string candidate = Path.Combine(dir, stem + "." + label + (i > 1 ? "-" + i : "") + ext);
				if (!File.Exists(candidate) && !Directory.Exists(candidate))
					return candidate;
			}
		}

		static bool SameContent(string a, string b)
		{
			var fa = new FileInfo(a);
			var fb = new FileInfo(b);
			if (fa.Length != fb.Length)
				return false;
			using (FileStream sa = fa.OpenRead())
			using (FileStream sb = fb.OpenRead())
			{
				var ba = new byte[65536];
				var bb = new byte[65536];
				for (;;)
				{
					int na = sa.Read(ba, 0, ba.Length);
					int nb = sb.Read(bb, 0, bb.Length);
					if (na != nb)
						return false;
					if (na == 0)
						return true;
					for (int i = 0; i < na; i++)
						if (ba[i] != bb[i])
							return false;
				}
			}
		}

		// Removes `dir` and the empty folders under it; anything with a file in it stays.
		static void RemoveEmpty(string dir)
		{
			try
			{
				foreach (string sub in Directory.GetDirectories(dir))
					if ((File.GetAttributes(sub) & FileAttributes.ReparsePoint) == 0)
						RemoveEmpty(sub);
				if (Directory.GetFileSystemEntries(dir).Length == 0)
					Directory.Delete(dir, false);
			}
			catch (Exception)
			{
			}
		}

		static void WriteBackupNote(string backup)
		{
			try
			{
				File.WriteAllText(Path.Combine(backup, "README.txt"),
					"Files Garry's Mod servers saved in garrysmod\\data while you played on them, kept by the\r\n" +
					"Meowy Client Pack in a separate folder per server (named after its address) and moved here\r\n" +
					"when the pack was uninstalled. Garry's Mod doesn't read this folder. To give a server its\r\n" +
					"files back, copy that server's folder's contents into garrysmod\\data. Your own files (from\r\n" +
					"before the pack and from the main menu) were put back in garrysmod\\data.\r\n");
			}
			catch (Exception)
			{
			}
		}

		// What the uninstall did with the files, for the window and the console.
		public static string Describe(RestoreReport r)
		{
			if (r.Vaults.Count == 0)
				return "The pack had no files kept apart, so nothing in garrysmod\\data changed.";
			var sb = new System.Text.StringBuilder();
			sb.Append(r.ToData + " of your own file(s) put back in garrysmod\\data");
			if (r.Renamed > 0)
				sb.Append(" (" + r.Renamed + " under a new name such as name.before-pack.txt, because a different file " +
					"with that name was already there)");
			sb.Append(".");
			if (r.ToBackup > 0)
				sb.Append("\r\n" + r.ToBackup + " file(s) that servers saved are in \"" + r.BackupDir + "\", a folder per server.");
			if (r.Left.Count > 0)
			{
				sb.Append("\r\n" + r.Left.Count + " couldn't be moved and are still where they were, for example:");
				for (int i = 0; i < r.Left.Count && i < 3; i++)
					sb.Append("\r\n  " + r.Left[i]);
			}
			foreach (string v in r.KeptVaults)
				sb.Append("\r\nKept (not deleted, as it isn't empty): " + v);
			return sb.ToString();
		}

		public static void ExtractTo(string module, string path)
		{
			using (Stream src = Assembly.GetExecutingAssembly().GetManifestResourceStream(module))
			using (FileStream dst = File.Create(path))
				src.CopyTo(dst);
		}

		// --- Start-menu shortcut for notifications (src/notify_toast.h) ---------
		//
		// Windows only shows toasts from a program that isn't a Store app under an
		// AppUserModelID that a Start-menu shortcut carries. The module shows its
		// notifications as "MeowyNetwork.ClientPack", so the installer makes
		// "Meowy Client Pack" in the player's own Start menu (no admin rights)
		// with that ID; it starts Garry's Mod through Steam. Install makes it
		// again every time (which also repairs a broken one), uninstall removes
		// it. Without it the module falls back to a tray balloon.

		public const string AppUserModelId = "MeowyNetwork.ClientPack";   // = notify.h kAppId
		public const string ShortcutFileName = "Meowy Client Pack.lnk";   // = notify.h kShortcutName
		const string ShortcutLaunch = "steam://rungameid/4000";

		public static string StartMenuShortcut()
		{
			return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), ShortcutFileName);
		}

		// Creates (or replaces) the shortcut at `lnk`. Its icon is the module's
		// own (gmcl_meowy_win64.dll, icon 0) when that file is in `garrysmod`.
		public static void CreateShortcut(string lnk, string garrysmod)
		{
			string dir = Path.GetDirectoryName(lnk);
			if (!string.IsNullOrEmpty(dir))
				Directory.CreateDirectory(dir);
			if (File.Exists(lnk))
				File.Delete(lnk);

			var link = (Shell.IShellLinkW)new Shell.CShellLink();
			try
			{
				string explorer = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "explorer.exe");
				link.SetPath(explorer);
				link.SetArguments(ShortcutLaunch);
				link.SetDescription("Garry's Mod with the Meowy Client Pack (shows Meowy Network's notifications)");
				string icon = string.IsNullOrEmpty(garrysmod) ? null : Path.Combine(garrysmod, @"lua\bin\gmcl_meowy_win64.dll");
				if (icon != null && File.Exists(icon))
					link.SetIconLocation(icon, 0);

				var store = (Shell.IPropertyStore)link;
				Shell.PropertyKey key = Shell.AppUserModelIdKey;
				var value = new Shell.PropVariant();
				value.vt = Shell.VT_LPWSTR;
				value.p = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUni(AppUserModelId);
				try
				{
					System.Runtime.InteropServices.Marshal.ThrowExceptionForHR(store.SetValue(ref key, ref value));
					System.Runtime.InteropServices.Marshal.ThrowExceptionForHR(store.Commit());
				}
				finally
				{
					Shell.PropVariantClear(ref value);
				}
				((System.Runtime.InteropServices.ComTypes.IPersistFile)link).Save(lnk, true);
			}
			finally
			{
				System.Runtime.InteropServices.Marshal.ReleaseComObject(link);
			}
		}

		// The AppUserModelID the shortcut at `lnk` carries, or null (no file, not
		// a shortcut, or no ID).
		public static string ShortcutAppId(string lnk)
		{
			if (!File.Exists(lnk))
				return null;
			var link = (Shell.IShellLinkW)new Shell.CShellLink();
			try
			{
				((System.Runtime.InteropServices.ComTypes.IPersistFile)link).Load(lnk, 0);
				var store = (Shell.IPropertyStore)link;
				Shell.PropertyKey key = Shell.AppUserModelIdKey;
				Shell.PropVariant value;
				if (store.GetValue(ref key, out value) != 0)
					return null;
				try
				{
					return value.vt == Shell.VT_LPWSTR ? System.Runtime.InteropServices.Marshal.PtrToStringUni(value.p) : null;
				}
				finally
				{
					Shell.PropVariantClear(ref value);
				}
			}
			catch (Exception)
			{
				return null;
			}
			finally
			{
				System.Runtime.InteropServices.Marshal.ReleaseComObject(link);
			}
		}

		// What the shortcut at `lnk` starts (path and arguments), for tests.
		public static string ShortcutTarget(string lnk)
		{
			var link = (Shell.IShellLinkW)new Shell.CShellLink();
			try
			{
				((System.Runtime.InteropServices.ComTypes.IPersistFile)link).Load(lnk, 0);
				var path = new System.Text.StringBuilder(1024);
				var args = new System.Text.StringBuilder(1024);
				link.GetPath(path, path.Capacity, IntPtr.Zero, 0);
				link.GetArguments(args, args.Capacity);
				return path + " " + args;
			}
			finally
			{
				System.Runtime.InteropServices.Marshal.ReleaseComObject(link);
			}
		}

		public static void RemoveShortcut(string lnk)
		{
			if (File.Exists(lnk))
				File.Delete(lnk);
		}

		// Install's part: never fails the install, says what happened.
		public static string InstallShortcut(string garrysmod)
		{
			try
			{
				string lnk = StartMenuShortcut();
				CreateShortcut(lnk, garrysmod);
				return "notifications: Start-menu shortcut " + lnk;
			}
			catch (Exception e)
			{
				return "notifications: couldn't make the Start-menu shortcut (" + e.Message + "); they'll show as tray balloons";
			}
		}

		public static string UninstallShortcut()
		{
			try
			{
				string lnk = StartMenuShortcut();
				RemoveShortcut(lnk);
				return "removed " + lnk;
			}
			catch (Exception e)
			{
				return "couldn't remove the Start-menu shortcut: " + e.Message;
			}
		}
	}

	// The shell's COM interfaces for the shortcut (no extra references needed).
	static class Shell
	{
		public const ushort VT_LPWSTR = 31;

		// System.AppUserModel.ID
		public static readonly PropertyKey AppUserModelIdKey = new PropertyKey
		{
			fmtid = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"),
			pid = 5,
		};

		[System.Runtime.InteropServices.ComImport]
		[System.Runtime.InteropServices.Guid("00021401-0000-0000-C000-000000000046")]
		public class CShellLink
		{
		}

		[System.Runtime.InteropServices.ComImport]
		[System.Runtime.InteropServices.InterfaceType(System.Runtime.InteropServices.ComInterfaceType.InterfaceIsIUnknown)]
		[System.Runtime.InteropServices.Guid("000214F9-0000-0000-C000-000000000046")]
		public interface IShellLinkW
		{
			void GetPath([System.Runtime.InteropServices.Out, System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder pszFile, int cch, IntPtr pfd, uint fFlags);
			void GetIDList(out IntPtr ppidl);
			void SetIDList(IntPtr pidl);
			void GetDescription([System.Runtime.InteropServices.Out, System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder pszName, int cch);
			void SetDescription([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string pszName);
			void GetWorkingDirectory([System.Runtime.InteropServices.Out, System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder pszDir, int cch);
			void SetWorkingDirectory([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string pszDir);
			void GetArguments([System.Runtime.InteropServices.Out, System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder pszArgs, int cch);
			void SetArguments([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string pszArgs);
			void GetHotkey(out short pwHotkey);
			void SetHotkey(short wHotkey);
			void GetShowCmd(out int piShowCmd);
			void SetShowCmd(int iShowCmd);
			void GetIconLocation([System.Runtime.InteropServices.Out, System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] System.Text.StringBuilder pszIconPath, int cch, out int piIcon);
			void SetIconLocation([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string pszIconPath, int iIcon);
			void SetRelativePath([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string pszPathRel, uint dwReserved);
			void Resolve(IntPtr hwnd, uint fFlags);
			void SetPath([System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.LPWStr)] string pszFile);
		}

		[System.Runtime.InteropServices.ComImport]
		[System.Runtime.InteropServices.InterfaceType(System.Runtime.InteropServices.ComInterfaceType.InterfaceIsIUnknown)]
		[System.Runtime.InteropServices.Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
		public interface IPropertyStore
		{
			[System.Runtime.InteropServices.PreserveSig] int GetCount(out uint cProps);
			[System.Runtime.InteropServices.PreserveSig] int GetAt(uint iProp, out PropertyKey pkey);
			[System.Runtime.InteropServices.PreserveSig] int GetValue(ref PropertyKey key, out PropVariant pv);
			[System.Runtime.InteropServices.PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant pv);
			[System.Runtime.InteropServices.PreserveSig] int Commit();
		}

		[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential, Pack = 4)]
		public struct PropertyKey
		{
			public Guid fmtid;
			public uint pid;
		}

		// PROPVARIANT, only as far as a string goes (24 bytes: big enough for x64's).
		[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Explicit)]
		public struct PropVariant
		{
			[System.Runtime.InteropServices.FieldOffset(0)] public ushort vt;
			[System.Runtime.InteropServices.FieldOffset(8)] public IntPtr p;
			[System.Runtime.InteropServices.FieldOffset(16)] public long pad;
		}

		[System.Runtime.InteropServices.DllImport("ole32.dll")]
		public static extern int PropVariantClear(ref PropVariant pv);
	}

	class MainForm : Form
	{
		readonly TextBox m_path = new TextBox();
		readonly Label m_status = new Label();
		readonly Button m_install = new Button();
		readonly Button m_uninstall = new Button();
		readonly CheckBox m_startup = new CheckBox();

		public MainForm()
		{
			Text = "Meowy Client Pack " + Assembly.GetExecutingAssembly().GetName().Version.ToString(3);
			FormBorderStyle = FormBorderStyle.FixedDialog;
			MaximizeBox = false;
			StartPosition = FormStartPosition.CenterScreen;
			ClientSize = new Size(520, 280);
			Font = new Font("Segoe UI", 9f);

			var intro = new Label
			{
				Text = "For Meowy Network: faster joins, our own loading screen with games, Discord status, and " +
					"Windows notifications while you're alt-tabbed.\n" +
					"It only works on Meowy Network servers. Source code: " + Program.SourceUrl,
				Location = new Point(12, 12),
				Size = new Size(496, 52),
			};

			var pathLabel = new Label { Text = "Garry's Mod folder:", Location = new Point(12, 72), AutoSize = true };
			m_path.Location = new Point(12, 92);
			m_path.Size = new Size(410, 23);
			var browse = new Button { Text = "Browse...", Location = new Point(428, 91), Size = new Size(80, 25) };
			browse.Click += delegate { Browse(); };

			m_status.Location = new Point(12, 124);
			m_status.Size = new Size(496, 48);

			m_startup.Text = @"Start with the game (for faster Meowy joins; adds one line to lua\includes\menu.lua)";
			m_startup.Location = new Point(12, 180);
			m_startup.Size = new Size(496, 22);
			m_startup.Checked = true;

			m_install.Text = "Install";
			m_install.Location = new Point(12, 230);
			m_install.Size = new Size(110, 32);
			m_install.Click += delegate { Install(); };

			m_uninstall.Text = "Uninstall";
			m_uninstall.Location = new Point(130, 230);
			m_uninstall.Size = new Size(110, 32);
			m_uninstall.Click += delegate { Uninstall(); };

			var join = new Button { Text = "Join Meowy Network", Location = new Point(358, 230), Size = new Size(150, 32) };
			join.Click += delegate { Process.Start("steam://connect/" + Program.ServerAddress); };

			Controls.AddRange(new Control[] { intro, pathLabel, m_path, browse, m_status, m_startup, m_install, m_uninstall, join });

			m_path.Text = Program.FindGarrysMod() ?? "";
			m_path.TextChanged += delegate { RefreshStatus(); };
			RefreshStatus();
		}

		string BinDir()
		{
			return Path.Combine(m_path.Text.Trim(), @"lua\bin");
		}

		bool PathLooksRight()
		{
			string dir = m_path.Text.Trim();
			return dir.Length > 0 && Directory.Exists(Path.Combine(dir, "lua")) && File.Exists(Path.Combine(dir, "gameinfo.txt"));
		}

		void RefreshStatus()
		{
			if (!PathLooksRight())
			{
				m_status.Text = "Garry's Mod wasn't found. Pick its \"garrysmod\" folder (the one with gameinfo.txt).";
				m_install.Enabled = m_uninstall.Enabled = false;
				return;
			}
			bool installed = File.Exists(Path.Combine(BinDir(), Program.Modules[0])) || File.Exists(Path.Combine(BinDir(), Program.Modules[1]));
			bool hook = Program.StartupHookPresent(m_path.Text.Trim());
			m_status.Text = !installed ? "Not installed." :
				"Installed" + (hook ? ", starts with the game" : ", doesn't start with the game") + ". Install again to update.";
			if (installed)
				m_startup.Checked = hook;
			m_install.Enabled = true;
			m_uninstall.Enabled = installed;
		}

		void Browse()
		{
			using (var dlg = new FolderBrowserDialog { Description = "Select the \"garrysmod\" folder inside Garry's Mod" })
			{
				if (dlg.ShowDialog(this) == DialogResult.OK)
					m_path.Text = dlg.SelectedPath;
			}
		}

		bool GameClosed()
		{
			if (!Program.IsGameRunning())
				return true;
			MessageBox.Show(this, "Close Garry's Mod first: it keeps the module open while running.", Text,
				MessageBoxButtons.OK, MessageBoxIcon.Information);
			return false;
		}

		void Install()
		{
			if (!GameClosed())
				return;
			try
			{
				Directory.CreateDirectory(BinDir());
				foreach (string module in Program.Modules)
					Program.ExtractTo(module, Path.Combine(BinDir(), module));
				if (m_startup.Checked)
					Program.AddStartupHook(m_path.Text.Trim());
				else
					Program.RemoveStartupHook(m_path.Text.Trim());
				Program.InstallShortcut(m_path.Text.Trim());   // never fails the install (balloons without it)
				RefreshStatus();
				MessageBox.Show(this, "Installed. Join Meowy Network and your Discord status will show where you are.", Text,
					MessageBoxButtons.OK, MessageBoxIcon.Information);
			}
			catch (Exception e)
			{
				MessageBox.Show(this, "Couldn't install: " + e.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
			}
		}

		void Uninstall()
		{
			if (!GameClosed())
				return;
			string garrysmod = m_path.Text.Trim();
			var answer = MessageBox.Show(this,
				"Uninstall removes the pack, its start-up line and its own files.\r\n\r\n" +
				"Your garrysmod\\data files are put back first. While installed, the pack gave every server its own " +
				"garrysmod\\data and kept the rest apart:\r\n\r\n" +
				"- Your own files (everything from before the pack, and the main menu's) go back into garrysmod\\data. " +
				"Nothing there is overwritten: if a different file already has the name, both are kept, and the one " +
				"coming back gets a suffix such as name.before-pack.txt.\r\n\r\n" +
				"- Each server's own files (what that server saved while you played on it) are moved to\r\n\"" +
				Program.BackupDirFor(garrysmod) + "\", a folder per server, not into garrysmod\\data, where every " +
				"server's Lua could read them.\r\n\r\n" +
				"Nothing of yours is deleted. Uninstall now?",
				Text, MessageBoxButtons.OKCancel, MessageBoxIcon.Question);
			if (answer != DialogResult.OK)
				return;
			try
			{
				string files = Program.Describe(Program.RemoveData(garrysmod));
				foreach (string module in Program.Modules)
				{
					string file = Path.Combine(BinDir(), module);
					if (File.Exists(file))
						File.Delete(file);
				}
				Program.RemoveStartupHook(garrysmod);
				Program.UninstallShortcut();
				RefreshStatus();
				MessageBox.Show(this, "Uninstalled.\r\n\r\n" + files, Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
			}
			catch (Exception e)
			{
				MessageBox.Show(this, "Couldn't uninstall: " + e.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
			}
		}
	}
}
