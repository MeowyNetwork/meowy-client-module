<#
.SYNOPSIS
Measures what the pack changes, the same way every time: how long a join takes
and how smoothly the game runs, per configuration, on this PC.

.DESCRIPTION
For each configuration and run it starts Garry's Mod straight into a server
(+connect), times the join from the console log ("Connecting to" until the map
is drawn), lets the game settle, turns the view for -Seconds while the pack's
frame recorder measures (meowy_frames_status's numbers), leaves the server and
quits. Then it prints a table with the median of each configuration's runs and
writes it, with every run, to -OutDir (Markdown and JSON).

Configurations (-Configs, in this order each round):
  stock         the pack not loaded at all (-meowy_safe): join time only, since
                the frame recorder is the pack's
  pack          the pack as installed, your own settings
  no-speedups   the pack with every join speed-up off: join time against "pack"
  no-multicore  the pack with multicore rendering off: frames against "pack"

The first join after a restart of the PC reads everything from disk; later
ones come from Windows' file cache. So one join that isn't counted comes first
(-NoWarmUp skips it), and the rounds go through the configurations in turn
rather than one configuration at a time.

What it touches, and puts back: the pack's settings file
(%LOCALAPPDATA%\MeowyClientPack\menu_settings.txt) is copied before the first
run and restored at the end, also when a run fails (only when a configuration
changed a setting), and so is frames.txt (the recorder's kept sessions; -KeepSessions
leaves the benchmark's own in it). Two small files are written to garrysmod\cfg for
the camera turn and removed again. Nothing else is changed.

While it runs, leave the PC alone: the recorder only counts frames while the
game's window is in front, and the script brings it there.

.EXAMPLE
pwsh tools/bench/bench.ps1 -Game "D:\SteamLibrary\steamapps\common\GarrysMod" -Server 138.201.62.52:27015

.EXAMPLE
pwsh tools/bench/bench.ps1 -Game $g -Server $s -Configs pack, no-multicore -Runs 3 -Seconds 90
#>
param(
	[Parameter(Mandatory)][string]$Game,                      # the GarrysMod folder (it holds garrysmod\ and bin\)
	[Parameter(Mandatory)][string]$Server,                    # a.b.c.d:port
	[ValidateSet("stock", "pack", "no-speedups", "no-multicore")][string[]]$Configs = @("stock", "pack", "no-speedups", "no-multicore"),
	[ValidateRange(1, 20)][int]$Runs = 2,                     # counted runs per configuration
	[ValidateRange(35, 1800)][int]$Seconds = 60,              # measured play per run (the recorder keeps sessions of 30 s or more)
	[ValidateSet(32, 64)][int]$Bits = 64,
	[ValidateRange(30, 900)][int]$JoinTimeout = 300,
	[switch]$NoWarmUp,
	[switch]$NoTurn,                                          # stand still instead of turning the view
	[switch]$ListOnly,                                        # print what each configuration starts the game with, and stop
	[switch]$KeepSessions,                                    # leave the benchmark's sessions in the recorder's frames.txt
	[string]$OutDir = (Join-Path (Get-Location) "bench-out"),
	[string]$ExtraArgs = "-windowed -w 1600 -h 900"
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0   # a misspelled or missing variable or property is an error, not an empty value

if ($Server -notmatch '^\d{1,3}(\.\d{1,3}){3}:\d{1,5}$') { throw "Server must look like 1.2.3.4:27015." }
$exe = if ($Bits -eq 64) { Join-Path $Game "bin\win64\gmod.exe" } else { Join-Path $Game "bin\gmod.exe" }
if (-not (Test-Path $exe)) { throw "$exe not found: -Game is the GarrysMod folder, and -Bits 64 needs the x86-64 branch." }
$log = Join-Path $Game "garrysmod\console.log"
$cfg = Join-Path $Game "garrysmod\cfg"
$data = Join-Path $env:LOCALAPPDATA "MeowyClientPack"
$settingsFile = Join-Path $data "menu_settings.txt"
$framesFile = Join-Path $data "frames.txt"

# The settings each configuration sets (name = value); anything not named stays the player's own.
$SPEEDUPS = "meowy_fast_file_checks", "meowy_fast_lua_lookup", "meowy_fast_lua_find", "meowy_fast_fonts", "meowy_pak_cache",
	"meowy_prewarm", "meowy_prewarm_workshop", "meowy_join_boost"
$Setups = @{
	"stock" = @{ Args = "-meowy_safe"; Set = @{} }
	"pack" = @{ Args = ""; Set = @{} }   # nothing set: exactly what the player has
	"no-speedups" = @{ Args = ""; Set = @{ meowy_frame_recorder = "1" } }
	"no-multicore" = @{ Args = ""; Set = @{ meowy_frame_recorder = "1"; meowy_multicore = "0" } }
}
foreach ($n in $SPEEDUPS) { $Setups["no-speedups"].Set[$n] = "0" }

if ($ListOnly) {
	foreach ($name in $Configs) {
		$c = $Setups[$name]
		"{0,-13} launch option: {1,-12} settings: {2}" -f $name, $(if ($c.Args) { $c.Args } else { "(none)" }),
			$(if ($c.Set.Count) { ($c.Set.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join " " } else { "(your own)" })
	}
	return
}
if (Get-Process gmod -ErrorAction SilentlyContinue) { throw "Garry's Mod is running: close it first." }

# What the console must (not) say for a configuration to have been what it claims.
# Checked after every run: a configuration that didn't apply is a failed run,
# never a number in the table.
function Test-Applied([string]$name, [string[]]$lines) {
	$started = [bool]($lines -match '\[Meowy Client Pack\] \S+ started with the game')
	if ($name -eq "stock") {
		if ($started) { return "the pack was loaded" }
		return $null
	}
	if (-not $started) { return "the pack wasn't loaded" }
	if ($name -eq "no-multicore" -and ($lines -match 'multicore: gmod_mcore_test 1')) { return "multicore rendering was turned on" }
	if ($name -eq "no-speedups" -and ($lines -match 'prewarm: noted \d+ files')) { return "the join speed-ups were on" }
	return $null
}

Add-Type -Namespace Bench -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
'@

function Read-Since([long]$offset) {
	$fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
	try {
		[void]$fs.Seek($offset, 'Begin')
		$sr = New-Object System.IO.StreamReader($fs)
		return $sr.ReadToEnd() -split "`r?`n"
	}
	finally { $fs.Dispose() }
}
function Send-Game([string[]]$commands) {
	# The game's own way to hand a running copy a command line.
	Start-Process $exe -WorkingDirectory $Game -ArgumentList (@('-hijack') + $commands) -Wait
}
function Stop-Game {
	if (-not (Get-Process gmod -ErrorAction SilentlyContinue)) { return }
	Send-Game @('+quit')
	for ($i = 0; $i -lt 60 -and (Get-Process gmod -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
	if (Get-Process gmod -ErrorAction SilentlyContinue) { throw "The game didn't quit." }
}
function Set-Front {
	$p = Get-Process gmod -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
	if (-not $p) { return $false }
	[void][Bench.Win]::ShowWindow($p.MainWindowHandle, 9)
	[void][Bench.Win]::SetForegroundWindow($p.MainWindowHandle)
	Start-Sleep -Milliseconds 500
	return [Bench.Win]::GetForegroundWindow() -eq $p.MainWindowHandle
}
# The pack's settings as the file holds them, with these set.
function Write-Settings([string]$original, [hashtable]$set) {
	# Nothing to set and nothing set before: the file is left as it is.
	if ($set.Count -eq 0 -and -not $script:wroteSettings) { return }
	$script:wroteSettings = $true
	$seen = @{}
	$lines = @()
	foreach ($line in ($original -split "`r?`n")) {
		if ($line -match '^([A-Za-z0-9_]+)=') {
			$name = $Matches[1]
			if ($set.ContainsKey($name)) { $line = "$name=$($set[$name])" }
			$seen[$name] = $true
		}
		if ($line -ne "") { $lines += $line }
	}
	foreach ($name in $set.Keys) { if (-not $seen[$name]) { $lines += "$name=$($set[$name])" } }
	[System.IO.File]::WriteAllText($settingsFile, ($lines -join "`n"))
}
function Get-Median([double[]]$v) {
	if (-not $v -or $v.Count -eq 0) { return $null }
	$s = @($v | Sort-Object)
	$m = [int][math]::Floor($s.Count / 2)
	if ($s.Count % 2) { return $s[$m] }
	return ($s[$m - 1] + $s[$m]) / 2
}

function Invoke-Run([string]$name, [bool]$measure) {
	$config = $name
	$c = $Setups[$name]
	if (-not $c) { throw "no configuration named $name" }
	if ($config -ne "stock") { Write-Settings $script:settingsOriginal $c.Set }
	$offset = (Get-Item $log).Length
	$launch = @('-steam', '-game', 'garrysmod', '-condebug', '+con_timestamp', '1') + ($ExtraArgs -split ' ' | Where-Object { $_ }) +
		($c.Args -split ' ' | Where-Object { $_ }) + @('+connect', $Server)
	$started = Get-Date
	Start-Process $exe -WorkingDirectory $Game -ArgumentList $launch
	$result = [ordered]@{ config = $config; join = $null; fps = $null; low1 = $null; low01 = $null; median = $null; p99 = $null; hitches = $null; worst = $null; note = "" }
	try {
		# The join: from "Connecting to" until the map is drawn.
		$connectAt = $null; $spawnAt = $null; $front = 0; $asked = 0
		$deadline = (Get-Date).AddSeconds($JoinTimeout)
		while ((Get-Date) -lt $deadline -and -not $spawnAt) {
			Start-Sleep -Milliseconds 500
			if (-not (Get-Process gmod -ErrorAction SilentlyContinue)) { throw "the game closed while joining" }
			$lines = Read-Since $offset
			if (-not $connectAt -and ($lines -match 'Connecting to ')) { $connectAt = Get-Date }
			if ($lines -match 'Redownloading all lightmaps') { $spawnAt = Get-Date }
			if ($lines -match 'Disconnect: |Connection failed after') { throw "the join failed" }
		}
		if (-not $spawnAt) { throw "no spawn within $JoinTimeout s" }
		# The console's own timestamps where both lines have one (to the second); else this script's clock.
		$stamp = { param($pattern) $l = (Read-Since $offset | Where-Object { $_ -match $pattern } | Select-Object -First 1); if ($l -match '^(\d\d/\d\d/\d{4})(?: -)? (\d\d:\d\d:\d\d): ') { [datetime]::ParseExact($Matches[1] + ' ' + $Matches[2], 'MM/dd/yyyy HH:mm:ss', $null) } }
		$result.join = [math]::Round((($spawnAt) - ($(if ($connectAt) { $connectAt } else { $started }))).TotalSeconds, 1)
		$a = & $stamp 'Connecting to '; $b = & $stamp 'Redownloading all lightmaps'
		if ($a -and $b -and $b -ge $a -and [math]::Abs(($b - $a).TotalSeconds - $result.join) -gt 1.5) { $result.note = "console says $([int]($b - $a).TotalSeconds) s" }

		if ($measure -and $config -ne "stock") {
			Start-Sleep 8   # the recorder's own five seconds, and the server's first messages
			if (Set-Front) { $front++ }
			$asked++
			if (-not $NoTurn) { Send-Game @('+exec', 'meowy_bench_turn_on') ; [void](Set-Front) }
			$end = (Get-Date).AddSeconds($Seconds)
			while ((Get-Date) -lt $end) {
				Start-Sleep 5
				if (-not (Get-Process gmod -ErrorAction SilentlyContinue)) { throw "the game closed while measuring" }
				if (Set-Front) { $front++ }
				$asked++
			}
			# The recorder skips frames while another window is in front.
			if ($front -lt $asked) { $result.note = ($result.note + " in front $front of $asked checks").Trim() }
			if (-not $NoTurn) { Send-Game @('+exec', 'meowy_bench_turn_off') }
			$before = (Get-Item $log).Length
			Send-Game @('+disconnect')
			$line = $null
			for ($i = 0; $i -lt 20 -and -not $line; $i++) {
				Start-Sleep -Milliseconds 500
				$line = Read-Since $before | Where-Object { $_ -match 'frames on .*: (\d+) fps average, 1% low (\d+) fps, 0\.1% low (\d+) fps, median frame ([\d.]+) ms, 99% under ([\d.]+) ms, (\d+) hitches \(([\d.]+) a minute\), worst (\d+) ms' } | Select-Object -Last 1
			}
			if ($line -and $line -match 'frames on .*: (\d+) fps average, 1% low (\d+) fps, 0\.1% low (\d+) fps, median frame ([\d.]+) ms, 99% under ([\d.]+) ms, (\d+) hitches \(([\d.]+) a minute\), worst (\d+) ms') {
				$inv = [cultureinfo]::InvariantCulture
				$result.fps = [int]$Matches[1]; $result.low1 = [int]$Matches[2]; $result.low01 = [int]$Matches[3]
				$result.median = [double]::Parse($Matches[4], $inv); $result.p99 = [double]::Parse($Matches[5], $inv)
				$result.hitches = [int]$Matches[6]; $result.worst = [int]$Matches[8]
			}
			else { $result.note = ($result.note + " no frames line (pack older than 1.9.0, or the window wasn't in front)").Trim() }
		}
	}
	catch { $result.note = ($result.note + " FAILED: " + $_.Exception.Message).Trim() }
	finally { Stop-Game }
	$wrong = Test-Applied $config (Read-Since $offset)
	if ($wrong) {
		# Its numbers would be another configuration's: not kept.
		foreach ($field in "join", "fps", "low1", "low01", "median", "p99", "hitches", "worst") { $result[$field] = $null }
		$result.note = ($result.note + " FAILED: not the $config configuration ($wrong)").Trim()
	}
	return [pscustomobject]$result
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$script:wroteSettings = $false
$script:settingsOriginal = if (Test-Path $settingsFile) { [System.IO.File]::ReadAllText($settingsFile) } else { "" }
$framesOriginal = if (Test-Path $framesFile) { [System.IO.File]::ReadAllBytes($framesFile) } else { $null }
$turnOn = Join-Path $cfg "meowy_bench_turn_on.cfg"
$turnOff = Join-Path $cfg "meowy_bench_turn_off.cfg"
$results = @()
try {
	Set-Content $turnOn "+left" -Encoding ascii
	Set-Content $turnOff "-left" -Encoding ascii
	if (-not $NoWarmUp) {
		"warm-up join (not counted) ..."
		$warm = @($Configs | Where-Object { $_ -ne "stock" })[0]
		if (-not $warm) { $warm = "stock" }
		$w = Invoke-Run $warm $false
		"  joined in $($w.join) s $($w.note)"
	}
	for ($round = 1; $round -le $Runs; $round++) {
		foreach ($config in $Configs) {
			"round $round of ${Runs}: $config ..."
			$r = Invoke-Run $config $true
			$r | Add-Member round $round
			$results += $r
			"  join $($r.join) s" + $(if ($null -ne $r.fps) { ", $($r.fps) fps average, 1% low $($r.low1), $($r.hitches) hitches" } else { "" }) + $(if ($r.note) { "  [$($r.note)]" } else { "" })
		}
	}
}
finally {
	Remove-Item $turnOn, $turnOff -ErrorAction SilentlyContinue
	if (Test-Path $data) {
		if ($script:wroteSettings) {
			if ($script:settingsOriginal -ne "") { [System.IO.File]::WriteAllText($settingsFile, $script:settingsOriginal) }
			elseif (Test-Path $settingsFile) { Remove-Item $settingsFile }
		}
		if (-not $KeepSessions) {
			if ($null -ne $framesOriginal) { [System.IO.File]::WriteAllBytes($framesFile, $framesOriginal) }
			elseif (Test-Path $framesFile) { Remove-Item $framesFile }
		}
	}
}

# The table: the median of each configuration's runs.
$packVersion = (Get-Item (Join-Path $Game "garrysmod\lua\bin\gmcl_meowy_win$Bits.dll") -ErrorAction SilentlyContinue).VersionInfo.FileVersion
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$gpu = (Get-CimInstance Win32_VideoController | Sort-Object AdapterRAM -Descending | Select-Object -First 1).Name
$fmt = { param($v, $unit) if ($null -eq $v) { "" } else { "$v$unit" } }
$md = @()
$md += "# Meowy Client Pack benchmark, $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
$md += ""
$md += "- Server: $Server. Game: $Bits-bit. Pack in lua\bin: $packVersion (a downloaded update, if any, is what runs)."
$md += "- $cpu; $gpu."
$md += "- $Runs counted run(s) per configuration, $Seconds s measured each$(if ($NoTurn) { ', standing still' } else { ', turning the view' }); the table is the median."
$md += ""
$md += "| Configuration | Join | Join, fastest to slowest | Average | 1% low | 0.1% low | Median frame | 99% under | Hitches | Worst |"
$md += "|---|---|---|---|---|---|---|---|---|---|"
foreach ($config in $Configs) {
	$mine = $results | Where-Object { $_.config -eq $config }
	$med = { param($field) $v = @($mine | Where-Object { $null -ne $_.$field } | ForEach-Object { [double]$_.$field }); if ($v.Count) { [math]::Round((Get-Median $v), 1) } }
	# The spread says how much of a difference between two rows is just run-to-run noise.
	$joins = @($mine | Where-Object { $null -ne $_.join } | ForEach-Object { [double]$_.join } | Sort-Object)
	$spread = if ($joins.Count -gt 1) { "$($joins[0]) to $($joins[-1]) s" } else { "" }
	$md += "| $config | $(& $fmt (& $med 'join') ' s') | $spread | $(& $fmt (& $med 'fps') ' fps') | $(& $fmt (& $med 'low1') ' fps') | $(& $fmt (& $med 'low01') ' fps') | $(& $fmt (& $med 'median') ' ms') | $(& $fmt (& $med 'p99') ' ms') | $(& $fmt (& $med 'hitches') '') | $(& $fmt (& $med 'worst') ' ms') |"
}
$md += ""
$md += "Every run:"
$md += ""
$md += "| Round | Configuration | Join | Average | 1% low | Hitches | Worst | Note |"
$md += "|---|---|---|---|---|---|---|---|"
foreach ($r in $results) { $md += "| $($r.round) | $($r.config) | $(& $fmt $r.join ' s') | $(& $fmt $r.fps ' fps') | $(& $fmt $r.low1 ' fps') | $(& $fmt $r.hitches '') | $(& $fmt $r.worst ' ms') | $($r.note) |" }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$mdPath = Join-Path $OutDir "bench-$stamp.md"
$md | Set-Content $mdPath -Encoding utf8
@{ at = (Get-Date).ToString('o'); server = $Server; bits = $Bits; pack = $packVersion; cpu = $cpu; gpu = $gpu; seconds = $Seconds; runs = $results } |
	ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutDir "bench-$stamp.json") -Encoding utf8
""
$md | Select-Object -Skip ([array]::IndexOf($md, ($md | Where-Object { $_ -like '| Configuration*' } | Select-Object -First 1))) -First ($Configs.Count + 2)
""
"written to $mdPath"
if ($results | Where-Object { $_.note -match 'FAILED' }) { exit 1 }
