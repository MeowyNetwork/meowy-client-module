# Runs every test program a build tree has (CMakeLists.txt's meowy_smoke list,
# written to <Build>\smokes.txt), one after another, each with a fresh empty
# folder when it takes one. Prints one line per test; a failing test's output
# is shown, the rest still run, and the exit code says whether all passed.
#
#   cmake -S . -B buildtest -A x64 -DMEOWY_BUILD_TESTS=ON
#   cmake --build buildtest --config Release --parallel
#   pwsh tests/run_smokes.ps1 -Build buildtest
#
# -Only <name>...: just those. CI runs this once per architecture.
param(
	[Parameter(Mandatory)][string]$Build,
	[string]$Config = "Release",
	[string[]]$Only
)
$ErrorActionPreference = "Stop"
$list = Join-Path $Build "smokes.txt"
if (-not (Test-Path $list)) { throw "$list not found: configure with -DMEOWY_BUILD_TESTS=ON first." }
$scratchRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("meowy-smokes-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $scratchRoot | Out-Null

$failed = @()
$ran = 0
$all = [System.Diagnostics.Stopwatch]::StartNew()
try {
	foreach ($line in Get-Content $list) {
		$name, $kind = $line.Trim() -split ' ', 2
		if (-not $name) { continue }
		if ($Only -and $Only -notcontains $name) { continue }
		$exe = Join-Path $Build "$Config\$name.exe"
		if (-not (Test-Path $exe)) {
			"{0,-26} MISSING ({1})" -f $name, $exe
			$failed += $name
			continue
		}
		$testArgs = @()
		if ($kind -eq "scratch") {
			$dir = New-Item -ItemType Directory -Force (Join-Path $scratchRoot $name)
			$testArgs = @($dir.FullName)
		}
		$t = [System.Diagnostics.Stopwatch]::StartNew()
		$ErrorActionPreference = "Continue"   # a test's stderr isn't this script's error
		$out = & $exe @testArgs 2>&1
		$code = $LASTEXITCODE
		$ErrorActionPreference = "Stop"
		$ran++
		$last = ($out | Select-Object -Last 1)
		"{0,-26} {1,-4} {2,5:n1} s  {3}" -f $name, $(if ($code -eq 0) { "ok" } else { "FAIL" }), $t.Elapsed.TotalSeconds, $last
		if ($code -ne 0) {
			$failed += $name
			$out | Select-Object -Last 60 | ForEach-Object { "    $_" }
			"    (exit code $code)"
		}
	}
}
finally {
	Remove-Item -Recurse -Force $scratchRoot -ErrorAction SilentlyContinue
}
""
if ($ran -eq 0 -and -not $failed) { throw "No test ran." }
if ($failed) {
	"{0} failed ({1} ran): {2}" -f $failed.Count, $ran, ($failed -join ", ")
	exit 1
}
"all {0} passed in {1:n0} s" -f $ran, $all.Elapsed.TotalSeconds
exit 0
