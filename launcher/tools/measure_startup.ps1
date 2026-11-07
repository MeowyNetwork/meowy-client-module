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
		$start = (Get-Date).AddSeconds(-1)
		$sw = [Diagnostics.Stopwatch]::StartNew()
		if ($mode -eq 'steam') { Start-Process $steam -ArgumentList '-applaunch', '4000' }
		else { Start-Process $Launcher -ArgumentList '--play' }
		$proc = $null; $pack = $null
		while ($sw.Elapsed.TotalSeconds -lt 90 -and $null -eq $pack) {
			if ($null -eq $proc -and (Get-Process gmod -ErrorAction SilentlyContinue)) { $proc = $sw.Elapsed.TotalSeconds }
			if (-not (Test-Path $marker)) { $pack = $sw.Elapsed.TotalSeconds }
			Start-Sleep -Milliseconds 50
		}
		$game = Get-CimInstance Win32_Process -Filter "Name='gmod.exe'" | Where-Object { $_.CommandLine -notmatch '--type=' } | Select-Object -First 1
		# Valid only if the game this run started is the one that ran the pack:
		# one game process, created after the clock started.
		$created = if ($game) { ([datetime]$game.CreationDate) } else { $null }
		$valid = $null -ne $pack -and $null -ne $proc -and $proc -gt 0.3 -and $created -and $created -ge $start
		$row = [pscustomobject]@{
			round = $r; mode = $mode; valid = $valid
			process_s = [math]::Round($proc, 2); pack_s = [math]::Round($pack, 2)
			game_to_pack_s = if ($valid) { [math]::Round($pack - $proc, 2) } else { $null }
			lua_refresh_off = [bool]($game.CommandLine -match '-disableluarefresh')
		}
		$row | Format-Table -HideTableHeaders | Out-String | ForEach-Object { $_.Trim() }
		$rows += $row
		Start-Sleep 6
		Get-Process gmod -ErrorAction SilentlyContinue | Stop-Process -Force
	}
}
Wait-GameClosed
Remove-Item $marker -ErrorAction SilentlyContinue
$rows | Export-Csv -NoTypeInformation $Out

function Median($xs) {
	$s = @($xs | Sort-Object)
	if ($s.Count -eq 0) { return $null }
	if ($s.Count % 2) { return $s[[int][math]::Floor($s.Count / 2)] }
	return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}
""
"Medians of valid runs (seconds). click->game: Steam's hop (and the launcher's, for its modes);"
"game->pack: the game's own start-up until the pack's menu Lua ran."
"mode           n   click->game   game->pack   click->pack"
foreach ($g in $rows | Group-Object mode) {
	$v = @($g.Group | Where-Object { $_.valid })
	"{0,-14} {1,-3} {2,10:N2}   {3,10:N2}   {4,10:N2}" -f $g.Name, $v.Count,
		(Median ($v | ForEach-Object { $_.process_s })), (Median ($v | ForEach-Object { $_.game_to_pack_s })), (Median ($v | ForEach-Object { $_.pack_s }))
}
