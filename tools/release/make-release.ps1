<#
.SYNOPSIS
Makes a signed release of the pack for the updater (src/updater.h): the
release document, its signature, and the files to upload, in one folder laid
out like the site (pack/v1/...).

.DESCRIPTION
- Hashes each DLL (SHA-256) and writes release-stable.txt with version, seq,
  expiry, minimum stub, rollout, a fresh salt and one "file" line per
  architecture (size, hash, path under /pack/v1/bin/<version>/).
- Signs it with the release key in the TPM (asks for its PIN); only that key
  can sign a release. Checks the signature before writing release-stable.sig.
- Copies the DLLs to pack/v1/bin/<version>/.

Upload the whole pack/v1 folder to meowynetwork.com: the binaries first, the
release document and its signature last. Players' packs take it within six
hours (and at their next start), from the next time they open the game.
To roll out in steps, sign again with a higher -Seq and a bigger -Rollout.

.EXAMPLE
pwsh tools/release/make-release.ps1 -Version 1.7.0 -Seq 1 -Win64 build-rel64\Release\gmcl_meowy_win64.dll -Win32 build-rel32\Release\gmcl_meowy_win32.dll
#>
param(
	[Parameter(Mandatory)][string]$Version,
	[Parameter(Mandatory)][long]$Seq,
	[Parameter(Mandatory)][string]$Win64,
	[Parameter(Mandatory)][string]$Win32,
	[ValidateRange(0, 100)][int]$Rollout = 100,
	[ValidateRange(1, 365)][int]$Days = 30,
	[ValidateRange(1, 99)][int]$MinStub = 1,
	[string]$OutDir = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) "release-out")
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security.Cryptography

if ($Version -notmatch '^\d{1,5}(\.\d{1,5}){0,3}$') { throw "Version must look like 1.7.0." }
if ($Seq -lt 1) { throw "Seq must be 1 or more, and higher than the last published release document's." }

$bin = Join-Path $OutDir "pack\v1\bin\$Version"
New-Item -ItemType Directory -Force $bin | Out-Null
$lines = @("meowy-pack-release 1", "seq $Seq", "version $Version",
	"expires $([DateTimeOffset]::UtcNow.AddDays($Days).ToUnixTimeSeconds())", "min_stub $MinStub", "rollout $Rollout")
$salt = -join ((1..8) | ForEach-Object { "{0:x2}" -f (Get-Random -Maximum 256) })
$lines += "salt $salt"
$sources = @{ win64 = $Win64; win32 = $Win32 }
foreach ($arch in "win64", "win32") {
	$src = (Resolve-Path $sources[$arch]).Path
	$name = "gmcl_meowy_$arch.dll"
	Copy-Item $src (Join-Path $bin $name) -Force
	$size = (Get-Item $src).Length
	$hash = (Get-FileHash $src -Algorithm SHA256).Hash.ToLowerInvariant()
	$lines += "file $arch $size $hash /pack/v1/bin/$Version/$name"
}
$body = ($lines -join "`n") + "`n"
$bytes = [System.Text.Encoding]::UTF8.GetBytes($body)

# The release key: in the TPM (new-keys.ps1's default), else the user's key store.
$tpm = [System.Security.Cryptography.CngProvider]::new("Microsoft Platform Crypto Provider")
$soft = [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
$provider = if ([System.Security.Cryptography.CngKey]::Exists("MeowyPackPolicyRelease", $tpm)) { $tpm } else { $soft }
$signer = [System.Security.Cryptography.ECDsaCng]::new([System.Security.Cryptography.CngKey]::Open("MeowyPackPolicyRelease", $provider))
$sig = $signer.SignData($bytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)
if ($sig.Length -ne 64 -or -not $signer.VerifyData($bytes, $sig, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) { throw "The signature didn't verify." }

$v1 = Join-Path $OutDir "pack\v1"
[System.IO.File]::WriteAllBytes((Join-Path $v1 "release-stable.txt"), $bytes)
[System.IO.File]::WriteAllText((Join-Path $v1 "release-stable.sig"), (-join ($sig | ForEach-Object { "{0:x2}" -f $_ })) + "`n")
Write-Host "Release $Version (seq $Seq, rollout $Rollout%, expires in $Days days) is ready in $v1."
Write-Host "Upload pack/v1/bin/$Version first, then release-stable.txt and release-stable.sig."
