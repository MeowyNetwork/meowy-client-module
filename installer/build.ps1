# Builds MeowyClientPack.exe with the .NET Framework compiler that ships with
# Windows, embedding the two module DLLs.
#   installer/build.ps1 -Win32 <path to gmcl_meowy_win32.dll> -Win64 <path to gmcl_meowy_win64.dll> -Out <exe>
param(
	[Parameter(Mandatory)] [string]$Win32,
	[Parameter(Mandatory)] [string]$Win64,
	[string]$Out = "MeowyClientPack.exe"
)
$ErrorActionPreference = 'Stop'

$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc" }

& $csc /nologo /target:winexe /optimize+ /out:$Out `
	/reference:System.Windows.Forms.dll /reference:System.Drawing.dll `
	"/resource:$Win32,gmcl_meowy_win32.dll" "/resource:$Win64,gmcl_meowy_win64.dll" `
	(Join-Path $PSScriptRoot 'Installer.cs')
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
Get-Item $Out
