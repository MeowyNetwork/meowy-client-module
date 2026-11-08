<#
.SYNOPSIS
Signs the pack's policy (src/policy.h): writes policy.sig next to policy.txt.

.DESCRIPTION
The signature is ECDSA P-256 over SHA-256 of the file's exact bytes, as 128 hex
digits (r || s): what policy::SignedBy checks. The file must already be in the
policy format (LF line ends, ending with one); this script checks the basics
and refuses otherwise, so a mistake is caught before anything is published.

- release (default): the TPM key made by new-keys.ps1 (asks for its PIN).
- ops / recovery: -KeyFile <the .key file>, asking for its password (a file
  without one still signs, with a warning to run protect-key.ps1). They can only make a policy
  stricter than the one players already have (the pack on stays on or goes
  off, nothing leaves off or revoked, min_version only rises), and the pack
  (1.7.1+) also refuses from them: a seq of 10^15 or more, a seq more than 1000
  above the kept one, and a notice other than the kept one. This script checks
  all of that against the published policy (or -Kept <its policy.txt>) and
  refuses to sign what every install would refuse, so a pause can't fail
  silently. Installs that kept an older policy than the published one compare
  against theirs; the published one is what nearly all have.

.EXAMPLE
pwsh tools/policy/sign-policy.ps1 policy.txt
pwsh tools/policy/sign-policy.ps1 policy.txt -Key ops -KeyFile $env:USERPROFILE\MeowyPackKeys\ops.key
pwsh tools/policy/sign-policy.ps1 policy.txt -Key ops -KeyFile ops.key -Kept published\policy.txt
#>
param(
	[Parameter(Mandatory, Position = 0)][string]$Policy,
	[ValidateSet("release", "ops", "recovery")][string]$Key = "release",
	[string]$KeyFile,
	# The policy players keep now (default: the one published at meowynetwork.com).
	[string]$Kept,
	# For tests: the key file's password, instead of asking.
	[securestring]$KeyPassword
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security.Cryptography
Import-Module (Join-Path $PSScriptRoot "PolicyKeyFile.psm1") -Force

$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Policy))
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($bytes.Length -eq 0 -or $bytes.Length -gt 8192) { throw "The policy must be 1 to 8192 bytes." }
if ($text.Contains("`r")) { throw "The policy has CR line ends; it must use LF only." }
if (-not $text.EndsWith("`n")) { throw "The policy must end with a line end." }
$lines = $text.TrimEnd("`n").Split("`n")
if ($lines[0] -ne "meowy-pack-policy 1") { throw "The first line must be: meowy-pack-policy 1" }
if (-not ($lines -match '^seq [1-9][0-9]{0,18}$')) { throw "The policy needs a seq line (higher than the last published one)." }
if (-not ($lines -match '^pack_enabled [01]$')) { throw "The policy needs a pack_enabled line." }

# A policy's fields (BigInteger: 19 digits can pass a long).
function Read-Fields([string]$t) {
	$f = @{ Seq = $null; Notice = ""; Enabled = $true; MinVersion = ""; Off = @(); Revoked = @() }
	foreach ($l in $t.TrimEnd("`n").Split("`n")) {
		if ($l -cmatch '^seq ([0-9]{1,19})$') { $f.Seq = [System.Numerics.BigInteger]::Parse($Matches[1]) }
		elseif ($l.StartsWith("notice ")) { $f.Notice = $l.Substring(7) }
		elseif ($l -cmatch '^pack_enabled ([01])$') { $f.Enabled = $Matches[1] -eq "1" }
		elseif ($l.StartsWith("min_version ")) { $f.MinVersion = $l.Substring(12) }
		elseif ($l.StartsWith("off ")) { $f.Off += $l.Substring(4) }
		elseif ($l.StartsWith("revoked ")) { $f.Revoked += $l.Substring(8) }
	}
	return $f
}
# "1.6.0" vs "1.10" as policy::CompareVersions: numeric parts, missing ones 0.
function Compare-Versions([string]$a, [string]$b) {
	$x = $a.Split("."); $y = $b.Split(".")
	for ($i = 0; $i -lt [Math]::Max($x.Count, $y.Count); $i++) {
		$p = if ($i -lt $x.Count) { [long]$x[$i] } else { 0 }
		$q = if ($i -lt $y.Count) { [long]$y[$i] } else { 0 }
		if ($p -ne $q) { return [Math]::Sign($p - $q) }
	}
	return 0
}
$next = Read-Fields $text

# The policy installs keep now: -Kept, else the published one.
$keptText = $null
if ($Kept) {
	$keptText = [System.IO.File]::ReadAllText((Resolve-Path $Kept))
} else {
	try {
		$r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "https://meowynetwork.com/pack/v1/policy.txt"
		$keptText = if ($r.Content -is [byte[]]) { [System.Text.Encoding]::UTF8.GetString($r.Content) } else { [string]$r.Content }
		if (-not $keptText.StartsWith("meowy-pack-policy 1`n")) { $keptText = $null }
	} catch {
		Write-Warning "Couldn't read the published policy ($($_.Exception.Message))."
	}
}
$keptPolicy = if ($keptText) { Read-Fields $keptText } else { $null }
if ($keptPolicy -and $keptPolicy.Seq -ne $null) {
	Write-Host "The published policy has seq $($keptPolicy.Seq)."
	if ($next.Seq -le $keptPolicy.Seq) { throw "seq $($next.Seq) isn't higher than the published $($keptPolicy.Seq): every install would refuse it." }
}

if ($Key -ne "release") {
	# The same limits as policy::MayReplace (src/policy.h kReleaseOnlySeq, kMaxStepSeq).
	$releaseOnly = [System.Numerics.BigInteger]::Parse("1000000000000000")
	$maxStep = [System.Numerics.BigInteger]1000
	if (-not $keptPolicy) { throw "The $Key key's limits are checked against the policy installs keep: the published one couldn't be read, so pass -Kept <its policy.txt>." }
	$base = if ($keptPolicy.Seq -ne $null) { $keptPolicy.Seq } else { [System.Numerics.BigInteger]::Zero }
	$limit = [System.Numerics.BigInteger]::Min($base + $maxStep, $releaseOnly - 1)
	Write-Host "The $Key key may sign seq $($base + 1) to $limit (and a fresh install with no policy kept takes seq 1000 at most)."
	if ($next.Seq -ge $releaseOnly) { throw "seq $($next.Seq) is in the release key's range (10^15 and up): every install refuses it from the $Key key." }
	if ($next.Seq -gt $limit) { throw "seq $($next.Seq) is more than 1000 above the kept $($base): every install refuses it from the $Key key. Use $limit or lower." }
	if ($next.Notice -ne "" -and $next.Notice -ne $keptPolicy.Notice) { throw "The $Key key can't set a notice (only keep the published one, or leave it out): every install refuses it. Leave the notice line out." }
	# Only stricter (policy::MayReplace, in its order and words).
	if ($next.Enabled -and -not $keptPolicy.Enabled) { throw "The $Key key can't switch the pack back on (the published policy has it off): only the release key can." }
	foreach ($o in $keptPolicy.Off) { if ($next.Off -cnotcontains $o) { throw "The $Key key can't switch a feature back on: keep the line 'off $o'." } }
	foreach ($v in $keptPolicy.Revoked) { if ($next.Revoked -cnotcontains $v) { throw "The $Key key can't un-revoke a version: keep the line 'revoked $v'." } }
	if ($keptPolicy.MinVersion -ne "" -and ($next.MinVersion -eq "" -or (Compare-Versions $next.MinVersion $keptPolicy.MinVersion) -lt 0)) {
		throw "The $Key key can't lower min_version: keep 'min_version $($keptPolicy.MinVersion)' or raise it."
	}
}

if ($Key -eq "release") {
	# In the TPM (new-keys.ps1's default), else the user's key store (-NoTpm).
	$tpm = [System.Security.Cryptography.CngProvider]::new("Microsoft Platform Crypto Provider")
	$soft = [System.Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
	$provider = if ([System.Security.Cryptography.CngKey]::Exists("MeowyPackPolicyRelease", $tpm)) { $tpm } else { $soft }
	$cng = [System.Security.Cryptography.CngKey]::Open("MeowyPackPolicyRelease", $provider)
	$signer = [System.Security.Cryptography.ECDsaCng]::new($cng)
} else {
	if (-not $KeyFile) { throw "-KeyFile is needed for the $Key key." }
	$signer = Import-PolicyKey (Resolve-Path $KeyFile).Path $KeyPassword
}
$sig = $signer.SignData($bytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)   # IEEE P1363: r || s
if ($sig.Length -ne 64) { throw "Unexpected signature length $($sig.Length)." }
if (-not $signer.VerifyData($bytes, $sig, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) { throw "The signature didn't verify." }
$hex = -join ($sig | ForEach-Object { "{0:x2}" -f $_ })
$out = [System.IO.Path]::ChangeExtension((Resolve-Path $Policy).Path, ".sig")   # policy.txt -> policy.sig
[System.IO.File]::WriteAllText($out, "$hex`n")
Write-Host "Signed with the $Key key: $out"
Write-Host "Publish both files at https://meowynetwork.com/pack/v1/policy.txt and policy.sig."
