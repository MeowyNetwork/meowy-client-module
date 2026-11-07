# Builds MeowyClientPack.exe with the .NET Framework compiler that ships with
# Windows, embedding the module DLLs, the start-up stubs and (optionally) the
# Meowy Launcher (launcher/, built with cargo).
#   installer/build.ps1 -Win32 <gmcl_meowy_win32.dll> -Win64 <gmcl_meowy_win64.dll>
#                       -Boot32 <gmsv_meowy_boot_win32.dll> -Boot64 <gmsv_meowy_boot_win64.dll>
#                       [-Launcher <MeowyLauncher.exe>] -Out <exe>
param(
	[Parameter(Mandatory)] [string]$Win32,
	[Parameter(Mandatory)] [string]$Win64,
	[Parameter(Mandatory)] [string]$Boot32,
	[Parameter(Mandatory)] [string]$Boot64,
	[string]$Launcher = "",
	[string]$Out = "MeowyClientPack.exe"
)
$ErrorActionPreference = 'Stop'

$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc" }

$extra = @()
# The pack's version (src/module.cpp VERSION), for the installed-version record
# the launcher's updater trusts (Installer.cs RecordInstalled).
$module = Join-Path $PSScriptRoot '..\src\module.cpp'
$m = Select-String -Path $module -Pattern 'const char\* const VERSION = "([0-9.]+)";' | Select-Object -First 1
if (-not $m) { throw "VERSION not found in $module" }
$versionFile = Join-Path ([IO.Path]::GetTempPath()) "meowy_pack_version_$PID.txt"
Set-Content -Path $versionFile -Value $m.Matches[0].Groups[1].Value -NoNewline
$extra += "/resource:$versionFile,pack_version.txt"
if ($Launcher) {
	if (-not (Test-Path $Launcher)) { throw "launcher not found at $Launcher" }
	$extra += "/resource:$Launcher,MeowyLauncher.exe"
}

& $csc /nologo /target:winexe /optimize+ /out:$Out `
	/reference:System.Windows.Forms.dll /reference:System.Drawing.dll `
	"/resource:$Win32,gmcl_meowy_win32.dll" "/resource:$Win64,gmcl_meowy_win64.dll" `
	"/resource:$Boot32,gmsv_meowy_boot_win32.dll" "/resource:$Boot64,gmsv_meowy_boot_win64.dll" `
	@extra (Join-Path $PSScriptRoot 'Installer.cs')
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
Get-Item $Out
