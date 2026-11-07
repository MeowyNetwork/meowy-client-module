# PROTOTYPE measurement harness for start-up #9 (launcher/DESIGN.md §9).
# Not part of the launcher. It starts bin\win64\gmod.exe directly (Steam must
# be running and signed in; no Steam launch flow), suspended, and either:
#   plain: resumes it as is, or
#   early: loads the INSTALLED pack (garrysmod\lua\bin\gmcl_meowy_win64.dll,
#          the same file the start-up stub loads later, so there's one copy),
#          calls its meowy_early_start export (src/early_entry.cpp), resumes.
# Modes alternate; the clock runs from ResumeThread to the pack's menu Lua
# removing on_server.txt (planted unparseable, so no prompt). The game is
# ended after each run. -LazyFonts moves the Workshop font cache aside in both
# modes (as the launcher's Faster start does) and puts it back after each run.
#
# This is the one place anything here writes into the game's memory (the DLL
# path, and a remote LoadLibraryW thread). Only on this PC, only to measure.
param([int]$Rounds = 5, [switch]$LazyFonts, [int]$Settle = 8, [string]$Out = "early_entry.csv")
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class Early
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO { public int cb; public string r, d, t; public int x, y, w, h, cx, cy, fa, flags; public short sw, r2; public IntPtr r3, i, o, e; }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int pid, tid; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint ResumeThread(IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr VirtualAllocEx(IntPtr p, IntPtr a, UIntPtr s, uint t, uint pr);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool WriteProcessMemory(IntPtr p, IntPtr a, byte[] b, UIntPtr s, out UIntPtr w);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr CreateRemoteThread(IntPtr p, IntPtr a, UIntPtr s, IntPtr f, IntPtr arg, uint fl, out int id);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string n);
    [DllImport("kernel32.dll", CharSet = CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m, string n);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibraryExW(string f, IntPtr h, uint fl);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr m);
    [DllImport("psapi.dll", SetLastError = true)] static extern bool EnumProcessModulesEx(IntPtr p, [Out] IntPtr[] m, int cb, out int need, uint filter);
    [DllImport("psapi.dll", CharSet = CharSet.Unicode)] static extern int GetModuleFileNameExW(IntPtr p, IntPtr m, StringBuilder s, int n);

    // Starts exe suspended; returns the process id. early: load dll and call export first.
    public static int Start(string exe, string args, string dir, bool early, string dll, string export, out string note)
    {
        note = "";
        var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si);
        PROCESS_INFORMATION pi;
        var cmd = new StringBuilder("\"" + exe + "\" " + args);
        if (!CreateProcessW(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, 0x4 /*CREATE_SUSPENDED*/, IntPtr.Zero, dir, ref si, out pi))
            throw new Exception("CreateProcess failed " + Marshal.GetLastWin32Error());
        try
        {
            if (early)
            {
                var sw = Stopwatch.StartNew();
                byte[] path = Encoding.Unicode.GetBytes(dll + "\0");
                IntPtr mem = VirtualAllocEx(pi.hProcess, IntPtr.Zero, (UIntPtr)path.Length, 0x3000, 0x04);
                UIntPtr w;
                WriteProcessMemory(pi.hProcess, mem, path, (UIntPtr)path.Length, out w);
                IntPtr loadLib = GetProcAddress(GetModuleHandleW("kernel32.dll"), "LoadLibraryW");
                int id;
                IntPtr t = CreateRemoteThread(pi.hProcess, IntPtr.Zero, UIntPtr.Zero, loadLib, mem, 0, out id);
                if (t == IntPtr.Zero) throw new Exception("CreateRemoteThread(LoadLibraryW) failed " + Marshal.GetLastWin32Error());
                WaitForSingleObject(t, 10000); CloseHandle(t);
                IntPtr remote = FindModule(pi.hProcess, dll);
                if (remote == IntPtr.Zero) throw new Exception("the pack didn't load in the new process");
                IntPtr local = LoadLibraryExW(dll, IntPtr.Zero, 0x1 /*DONT_RESOLVE_DLL_REFERENCES*/);
                long rva = GetProcAddress(local, export).ToInt64() - local.ToInt64();
                FreeLibrary(local);
                if (rva <= 0) throw new Exception("no export " + export);
                t = CreateRemoteThread(pi.hProcess, IntPtr.Zero, UIntPtr.Zero, new IntPtr(remote.ToInt64() + rva), IntPtr.Zero, 0, out id);
                if (t == IntPtr.Zero) throw new Exception("CreateRemoteThread(export) failed " + Marshal.GetLastWin32Error());
                WaitForSingleObject(t, 10000); CloseHandle(t);
                note = "injected in " + sw.ElapsedMilliseconds + " ms";
            }
            ResumeThread(pi.hThread);
            return pi.pid;
        }
        finally { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }

    static IntPtr FindModule(IntPtr p, string path)
    {
        var mods = new IntPtr[1024]; int need;
        if (!EnumProcessModulesEx(p, mods, mods.Length * IntPtr.Size, out need, 0x3)) return IntPtr.Zero;
        for (int i = 0; i < need / IntPtr.Size; i++)
        {
            var sb = new StringBuilder(1024);
            GetModuleFileNameExW(p, mods[i], sb, sb.Capacity);
            if (string.Equals(sb.ToString(), path, StringComparison.OrdinalIgnoreCase)) return mods[i];
        }
        return IntPtr.Zero;
    }
}
'@

$root = "C:\Program Files (x86)\Steam\steamapps\common\GarrysMod"
$exe = "$root\bin\win64\gmod.exe"
$dll = "$root\garrysmod\lua\bin\gmcl_meowy_win64.dll"
$fonts = "$root\garrysmod\cache\workshop\resource\fonts"
$aside = "$fonts.meowy-lazy"
$marker = "$env:LOCALAPPDATA\MeowyClientPack\on_server.txt"
$early = "$env:LOCALAPPDATA\MeowyClientPack\early.txt"
$gameArgs = "-steam -disableluarefresh -high -novid +exec autoexec.cfg"
$env:SteamAppId = "4000"; $env:SteamGameId = "4000"
if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { throw "Steam must be running" }
function Closed { for ($i = 0; $i -lt 60 -and (Get-Process gmod -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }; if (Get-Process gmod -ErrorAction SilentlyContinue) { throw "gmod still running" } }

$rows = @()
try {
	for ($r = 1; $r -le $Rounds; $r++) {
		foreach ($mode in 'plain', 'early') {
			Closed
			if ($LazyFonts -and (Test-Path $fonts)) { Rename-Item $fonts $aside }
			Remove-Item $early -ErrorAction SilentlyContinue
			Start-Sleep $Settle
			Set-Content $marker 'early_entry (not a server)' -NoNewline
			$sw = [Diagnostics.Stopwatch]::StartNew()
			$note = ""
			$id = [Early]::Start($exe, $gameArgs, $root, $mode -eq 'early', $dll, "meowy_early_start", [ref]$note)
			$resumed = $sw.Elapsed.TotalSeconds
			$pack = $null
			while ($sw.Elapsed.TotalSeconds -lt 90 -and $null -eq $pack) {
				if (-not (Test-Path $marker)) { $pack = $sw.Elapsed.TotalSeconds }
				Start-Sleep -Milliseconds 50
			}
			Start-Sleep 4
			$log = if (Test-Path $early) { (Get-Content $early) -join ' | ' } else { "" }
			$row = [pscustomobject]@{ round = $r; mode = $mode; resume_to_pack = if ($pack) { [math]::Round($pack - $resumed, 2) } else { $null }; note = $note; early = $log }
			"{0} {1,-5} {2,6}  {3}  {4}" -f $row.round, $row.mode, $row.resume_to_pack, $row.note, $row.early
			$rows += $row
			Get-Process gmod -ErrorAction SilentlyContinue | Stop-Process -Force
			Closed
			if (Test-Path $aside) { Rename-Item $aside $fonts }
		}
	}
} finally {
	Closed
	if (Test-Path $aside) { Rename-Item $aside $fonts }
	Remove-Item $marker -ErrorAction SilentlyContinue
}
$rows | Export-Csv -NoTypeInformation $Out
$pairs = 1..$Rounds | ForEach-Object { $a = $rows | Where-Object round -eq $_; $p = ($a | Where-Object mode -eq 'plain').resume_to_pack; $e = ($a | Where-Object mode -eq 'early').resume_to_pack; if ($p -and $e) { [math]::Round($e - $p, 2) } }
"paired (early minus plain): " + ($pairs -join ', ')
$s = @($pairs | Sort-Object); if ($s.Count) { "median: " + $s[[int][math]::Floor(($s.Count - 1) / 2)] }
