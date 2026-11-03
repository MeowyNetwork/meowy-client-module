# Builds MeowyClientPack.exe with the .NET Framework compiler that ships with
# Windows, embedding the module DLLs and the start-up stubs.
#   installer/build.ps1 -Win32 <gmcl_meowy_win32.dll> -Win64 <gmcl_meowy_win64.dll>
#                       -Boot32 <gmsv_meowy_boot_win32.dll> -Boot64 <gmsv_meowy_boot_win64.dll> -Out <exe>
param(
	[Parameter(Mandatory)] [string]$Win32,
	[Parameter(Mandatory)] [string]$Win64,
	[Parameter(Mandatory)] [string]$Boot32,
	[Parameter(Mandatory)] [string]$Boot64,
	[string]$Out = "MeowyClientPack.exe"
)
$ErrorActionPreference = 'Stop'

$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc" }

& $csc /nologo /target:winexe /optimize+ /out:$Out `
	/reference:System.Windows.Forms.dll /reference:System.Drawing.dll `
	"/resource:$Win32,gmcl_meowy_win32.dll" "/resource:$Win64,gmcl_meowy_win64.dll" `
	"/resource:$Boot32,gmsv_meowy_boot_win32.dll" "/resource:$Boot64,gmsv_meowy_boot_win64.dll" `
	(Join-Path $PSScriptRoot 'Installer.cs')
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
Get-Item $Out
