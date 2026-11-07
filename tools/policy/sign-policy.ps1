<#
.SYNOPSIS
Signs the pack's policy (src/policy.h): writes policy.sig next to policy.txt.

.DESCRIPTION
The signature is ECDSA P-256 over SHA-256 of the file's exact bytes, as 128 hex
digits (r || s): what policy::SignedBy checks. The file must already be in the
policy format (LF line ends, ending with one); this script checks the basics
and refuses otherwise, so a mistake is caught before anything is published.

- release (default): the TPM key made by new-keys.ps1 (asks for its PIN).
- ops / recovery: -KeyFile <the .key file>. They can only make a policy
  stricter than the one players already have; the pack refuses anything else.

.EXAMPLE
pwsh tools/policy/sign-policy.ps1 policy.txt
pwsh tools/policy/sign-policy.ps1 policy.txt -Key ops -KeyFile $env:USERPROFILE\MeowyPackKeys\ops.key
#>
param(
	[Parameter(Mandatory, Position = 0)][string]$Policy,
	[ValidateSet("release", "ops", "recovery")][string]$Key = "release",
	[string]$KeyFile
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security.Cryptography

$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Policy))
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($bytes.Length -eq 0 -or $bytes.Length -gt 8192) { throw "The policy must be 1 to 8192 bytes." }
if ($text.Contains("`r")) { throw "The policy has CR line ends; it must use LF only." }
if (-not $text.EndsWith("`n")) { throw "The policy must end with a line end." }
$lines = $text.TrimEnd("`n").Split("`n")
if ($lines[0] -ne "meowy-pack-policy 1") { throw "The first line must be: meowy-pack-policy 1" }
if (-not ($lines -match '^seq [1-9][0-9]{0,18}$')) { throw "The policy needs a seq line (higher than the last published one)." }
if (-not ($lines -match '^pack_enabled [01]$')) { throw "The policy needs a pack_enabled line." }

if ($Key -eq "release") {
	$cng = [System.Security.Cryptography.CngKey]::Open("MeowyPackPolicyRelease")
	$signer = [System.Security.Cryptography.ECDsaCng]::new($cng)
} else {
	if (-not $KeyFile) { throw "-KeyFile is needed for the $Key key." }
	$signer = [System.Security.Cryptography.ECDsa]::Create()
	$signer.ImportFromPem([System.IO.File]::ReadAllText((Resolve-Path $KeyFile)))
}
$sig = $signer.SignData($bytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)   # IEEE P1363: r || s
if ($sig.Length -ne 64) { throw "Unexpected signature length $($sig.Length)." }
if (-not $signer.VerifyData($bytes, $sig, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) { throw "The signature didn't verify." }
$hex = -join ($sig | ForEach-Object { "{0:x2}" -f $_ })
$out = [System.IO.Path]::ChangeExtension((Resolve-Path $Policy).Path, ".sig")   # policy.txt -> policy.sig
[System.IO.File]::WriteAllText($out, "$hex`n")
Write-Host "Signed with the $Key key: $out"
Write-Host "Publish both files at https://meowynetwork.com/pack/v1/policy.txt and policy.sig."
