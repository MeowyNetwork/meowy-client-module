// Meowy Client Pack installer.
//
// Finds Garry's Mod through Steam's own files, then copies the embedded
// gmcl_meowy_win32.dll and gmcl_meowy_win64.dll into garrysmod/lua/bin (both,
// so it works on either Steam branch). Uninstall removes exactly those two
// files. It touches nothing else and needs no admin rights when the Steam
// folder is writable, which is Steam's default.
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
[assembly: AssemblyVersion("1.0.0.0")]

namespace MeowyInstaller
{
	static class Program
	{
		public const string ServerAddress = "138.201.62.52:27015";
		public const string SourceUrl = "https://github.com/MeowyNetwork/meowy-client-module";
		public static readonly string[] Modules = { "gmcl_meowy_win32.dll", "gmcl_meowy_win64.dll" };

		// No arguments: the window. "--install" / "--uninstall" [path]: no window;
		// exit code 0 on success, 1 on failure, details on stdout.
		[STAThread]
		static int Main(string[] args)
		{
			if (args.Length > 0 && (args[0] == "--install" || args[0] == "--uninstall"))
				return RunCommandLine(args[0] == "--install", args.Length > 1 ? args[1] : FindGarrysMod());

			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);
			Application.Run(new MainForm());
			return 0;
		}

		// A winexe has no console of its own; print to the one that started it.
		[System.Runtime.InteropServices.DllImport("kernel32.dll")]
		static extern bool AttachConsole(int processId);

		static int RunCommandLine(bool install, string garrysmod)
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

		public static void ExtractTo(string module, string path)
		{
			using (Stream src = Assembly.GetExecutingAssembly().GetManifestResourceStream(module))
			using (FileStream dst = File.Create(path))
				src.CopyTo(dst);
		}
	}

	class MainForm : Form
	{
		readonly TextBox m_path = new TextBox();
		readonly Label m_status = new Label();
		readonly Button m_install = new Button();
		readonly Button m_uninstall = new Button();

		public MainForm()
		{
			Text = "Meowy Client Pack " + Assembly.GetExecutingAssembly().GetName().Version.ToString(3);
			FormBorderStyle = FormBorderStyle.FixedDialog;
			MaximizeBox = false;
			StartPosition = FormStartPosition.CenterScreen;
			ClientSize = new Size(520, 250);
			Font = new Font("Segoe UI", 9f);

			var intro = new Label
			{
				Text = "Adds Discord status for Meowy Network: your job, the player count and a button friends can use to join.\n" +
					"It only talks to the Discord app on this PC. Source code: " + Program.SourceUrl,
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

			m_install.Text = "Install";
			m_install.Location = new Point(12, 200);
			m_install.Size = new Size(110, 32);
			m_install.Click += delegate { Install(); };

			m_uninstall.Text = "Uninstall";
			m_uninstall.Location = new Point(130, 200);
			m_uninstall.Size = new Size(110, 32);
			m_uninstall.Click += delegate { Uninstall(); };

			var join = new Button { Text = "Join Meowy Network", Location = new Point(358, 200), Size = new Size(150, 32) };
			join.Click += delegate { Process.Start("steam://connect/" + Program.ServerAddress); };

			Controls.AddRange(new Control[] { intro, pathLabel, m_path, browse, m_status, m_install, m_uninstall, join });

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
			m_status.Text = installed ? "Installed. Install again to update." : "Not installed.";
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
			try
			{
				foreach (string module in Program.Modules)
				{
					string file = Path.Combine(BinDir(), module);
					if (File.Exists(file))
						File.Delete(file);
				}
				RefreshStatus();
			}
			catch (Exception e)
			{
				MessageBox.Show(this, "Couldn't uninstall: " + e.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
			}
		}
	}
}
