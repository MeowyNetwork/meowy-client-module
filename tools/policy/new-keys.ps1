<#
.SYNOPSIS
Creates the three keys of the pack's signed policy (src/policy.h) and prints the
public halves as C++ for src/policy.cpp.

.DESCRIPTION
- release: bound to this PC's TPM (Microsoft Platform Crypto Provider), can't be
  exported, and asks for a PIN on every use. It signs any policy. Run this on the
  PC that will sign releases, once. Without a TPM, -NoTpm keeps it in the user's
  key store instead (still not exportable).
- ops: written to <OutDir>\ops.key (PKCS#8). It may only make a policy stricter;
  it's meant for the backend's automatic pause.
- recovery: written to <OutDir>\recovery.key (PKCS#8). Also only stricter. Copy it
  somewhere offline (a USB stick in a drawer) and delete it from this PC.

Nothing here touches the network. Existing keys are never overwritten.

.EXAMPLE
pwsh tools/policy/new-keys.ps1
pwsh tools/policy/new-keys.ps1 -OutDir D:\keys -NoTpm
#>
param(
	[string]$OutDir = (Join-Path $env:USERPROFILE "MeowyPackKeys"),
	[switch]$NoTpm
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security.Cryptography

$releaseName = "MeowyPackPolicyRelease"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Get-PublicXY([System.Security.Cryptography.ECDsa]$ecdsa) {
	$q = $ecdsa.ExportParameters($false).Q
	return [byte[]]($q.X + $q.Y)
}

function Format-Cpp([string]$name, [byte[]]$xy) {
	$hex = ($xy | ForEach-Object { "0x{0:x2}" -f $_ })
	$lines = for ($i = 0; $i -lt 64; $i += 16) { "`t`t`t" + (($hex[$i..($i + 15)]) -join ", ") + "," }
	return "`t`tconst uint8_t $name[64] = {`n" + ($lines -join "`n") + "`n`t`t};"
}

# Release: TPM-bound (or user key store), non-exportable, PIN on use.
if ([System.Security.Cryptography.CngKey]::Exists($releaseName)) {
	throw "A key named $releaseName already exists in this account; not replacing it."
}
$params = [System.Security.Cryptography.CngKeyCreationParameters]::new()
$params.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::None
if (-not $NoTpm) {
	$params.Provider = [System.Security.Cryptography.CngProvider]::new("Microsoft Platform Crypto Provider")
}
$params.UIPolicy = [System.Security.Cryptography.CngUIPolicy]::new(
	[System.Security.Cryptography.CngUIProtectionLevels]::ForceHighProtection,
	"Meowy Client Pack policy release key", "Signs the pack's policy (kill switch).")
$cng = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256, $releaseName, $params)
$release = [System.Security.Cryptography.ECDsaCng]::new($cng)

# Ops and recovery: exportable files.
$files = @{}
foreach ($role in "ops", "recovery") {
	$path = Join-Path $OutDir "$role.key"
	if (Test-Path $path) { throw "$path already exists; not replacing it." }
	$key = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
	[System.IO.File]::WriteAllText($path, $key.ExportPkcs8PrivateKeyPem())
	$files[$role] = $key
}

Write-Host "Keys created. Release key: $releaseName ($(if ($NoTpm) { 'user key store' } else { 'TPM' })); ops and recovery keys in $OutDir."
Write-Host "Put these in src/policy.cpp (replacing the nullptr keys):`n"
Write-Output (Format-Cpp "kRelease" (Get-PublicXY $release))
Write-Output (Format-Cpp "kOps" (Get-PublicXY $files["ops"]))
Write-Output (Format-Cpp "kRecovery" (Get-PublicXY $files["recovery"]))
