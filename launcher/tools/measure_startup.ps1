# Click -> pack start-up, three ways, interleaved (launcher/DESIGN.md §11):
#   steam         steam.exe -applaunch 4000 (as the Steam library's Play)
#   launcher-off  MeowyLauncher.exe --play, "Faster start" off
#   launcher-on   MeowyLauncher.exe --play, "Faster start" on (-disableluarefresh)
#
# The launcher is running in the tray for every mode (as installed); for the
# launcher modes the clock starts when a second MeowyLauncher.exe is started,
# as a click on a shortcut does.
#
# Marker: the pack's menu Lua removes %LOCALAPPDATA%\MeowyClientPack\on_server.txt
# once per start (resilience.lua -> CrashedSession). A file the pack can't parse
# is planted before each run, so it never offers a rejoin; the time it's gone is
# "the pack's menu Lua ran", ~90% of the way to the menu (startup-menu-speed §1).
#
# Usage: launcher\tools\measure_startup.ps1 [-Rounds 4] [-Out startup.csv]
# The game must be closed. Each run ends by ending gmod.exe (WM_CLOSE doesn't
# quit at the menu).
param(
	[int]$Rounds = 4,
	[string]$Launcher = "$env:LOCALAPPDATA\MeowyClientPack\launcher\MeowyLauncher.exe",
	[string]$Out = "startup.csv",
	[int]$Settle = 8
)
$ErrorActionPreference = 'Stop'
$steam = ((Get-ItemProperty HKCU:\Software\Valve\Steam).SteamExe) -replace '/', '\'
$data = Join-Path $env:LOCALAPPDATA 'MeowyClientPack'
$marker = Join-Path $data 'on_server.txt'
$settings = Join-Path $data 'launcher\settings.json'
if (-not (Test-Path $Launcher)) { throw "launcher not found: $Launcher" }

function Wait-GameClosed {
	for ($i = 0; $i -lt 60 -and (Get-Process gmod -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
	if (Get-Process gmod -ErrorAction SilentlyContinue) { throw "gmod still running" }
}

function Restart-Launcher([bool]$faster) {
	if (Get-Process MeowyLauncher -ErrorAction SilentlyContinue) {
		& $Launcher --quit
		for ($i = 0; $i -lt 50 -and (Get-Process MeowyLauncher -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }
	}
	New-Item -ItemType Directory -Force (Split-Path $settings) | Out-Null
	Set-Content -Path $settings -Value ("{ ""disable_lua_refresh"": " + $faster.ToString().ToLower() + " }")
	Start-Process $Launcher -ArgumentList '--tray'
	Start-Sleep 3
}

$rows = @()
$current = $null
for ($r = 1; $r -le $Rounds; $r++) {
	foreach ($mode in 'steam', 'launcher-off', 'launcher-on') {
		Wait-GameClosed
		$want = $mode -eq 'launcher-on'
		if ($current -ne $want) { Restart-Launcher $want; $current = $want }
		Start-Sleep $Settle
		Set-Content -Path $marker -Value 'measure_startup (not a server)' -NoNewline
		$sw = [Diagnostics.Stopwatch]::StartNew()
		if ($mode -eq 'steam') { Start-Process $steam -ArgumentList '-applaunch', '4000' }
		else { Start-Process $Launcher -ArgumentList '--play' }
		$proc = $null; $pack = $null
		while ($sw.Elapsed.TotalSeconds -lt 90 -and $null -eq $pack) {
			if ($null -eq $proc -and (Get-Process gmod -ErrorAction SilentlyContinue)) { $proc = $sw.Elapsed.TotalSeconds }
			if (-not (Test-Path $marker)) { $pack = $sw.Elapsed.TotalSeconds }
			Start-Sleep -Milliseconds 50
		}
		$cmd = (Get-CimInstance Win32_Process -Filter "Name='gmod.exe'" | Where-Object { $_.CommandLine -notmatch '--type=' } | Select-Object -First 1).CommandLine
		$row = [pscustomobject]@{ round = $r; mode = $mode; process_s = [math]::Round($proc, 2); pack_s = [math]::Round($pack, 2); lua_refresh_off = [bool]($cmd -match '-disableluarefresh') }
		$row | Format-Table -HideTableHeaders | Out-String | ForEach-Object { $_.Trim() }
		$rows += $row
		Start-Sleep 6
		Get-Process gmod -ErrorAction SilentlyContinue | Stop-Process -Force
	}
}
Wait-GameClosed
Remove-Item $marker -ErrorAction SilentlyContinue
$rows | Export-Csv -NoTypeInformation $Out
""
"mode           n   process (s)   pack (s) mean   min-max"
foreach ($g in $rows | Group-Object mode) {
	$p = $g.Group | Where-Object { $_.pack_s -gt 0 }
	$m = ($p | Measure-Object pack_s -Average -Minimum -Maximum)
	$q = ($p | Measure-Object process_s -Average)
	"{0,-14} {1,-3} {2,8:N2}     {3,8:N2}        {4:N2}-{5:N2}" -f $g.Name, $m.Count, $q.Average, $m.Average, $m.Minimum, $m.Maximum
}
