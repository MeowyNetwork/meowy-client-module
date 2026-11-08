<#
.SYNOPSIS
Makes a signed release of the pack for the updater (src/updater.h): the
release document, its signature, and the files to upload, in one folder laid
out like the site (pack/v1/...).

.DESCRIPTION
- Hashes each DLL (SHA-256) and writes release-stable.txt with version, seq,
  expiry, minimum stub, rollout, the salt and one "file" line per
  architecture (size, hash, path under /pack/v1/bin/<version>/).
- The salt picks which installs are in a rollout. Signing the same version
  again (a longer expiry, a bigger rollout) keeps the published one, so the
  installs that had it stay in and the rollout only grows: -Salt, else the
  salt of the release-stable.txt published at meowynetwork.com (or, when the
  site can't be read, the one in -OutDir) when it names this version, else a
  new random one. When neither can be read, it stops unless -Salt or -NewSalt
  says which.
- -Seq must be above the published document's (the same seq only to sign the
  very same version and files again); -Offline skips reading it.
- Signs it with the release key in the TPM (asks for its PIN); only that key
  can sign a release. Checks the signature before writing release-stable.sig.
- Copies the DLLs to pack/v1/bin/<version>/.

Upload the whole pack/v1 folder to meowynetwork.com: the binaries first, the
release document and its signature last. Players' packs take it within six
hours (and at their next start), from the next time they open the game.
To roll out in steps, sign again with a higher -Seq and a bigger -Rollout (the
salt stays). The document expires after -Days (30): sign it again before then
so installs that haven't started the game yet still take it.

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
	[ValidatePattern('^[0-9a-f]{8,64}$')][string]$Salt,
	[switch]$NewSalt,
	# Sign without reading the published document (its seq and salt aren't checked).
	[switch]$Offline,
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
# The salt: kept for the same version (see above), new for a new one.
function SaltOf([string]$doc) {
	if ($doc -and $doc.StartsWith("meowy-pack-release 1`n") -and ($doc -cmatch "(?m)^version $([regex]::Escape($Version))$") -and ($doc -cmatch '(?m)^salt ([0-9a-f]{8,64})$')) { return $Matches[1] }
	return $null
}
# Lowercase: the pack and the launcher only parse lowercase hex (ValidatePattern ignores case).
$chosen = if ($Salt) { $Salt.ToLowerInvariant() } else { $null }
$from = "-Salt"
# What's published now: for the seq check below, and the salt.
$checked = $false   # whether we know what's published (so a new salt is right for a new version)
$published = $null
if (-not $Offline) {
	try {
		$published = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "https://meowynetwork.com/pack/v1/release-stable.txt").Content
		if ($published -is [byte[]]) { $published = [System.Text.Encoding]::UTF8.GetString($published) }
		$checked = $true
		if (-not $published.StartsWith("meowy-pack-release 1`n")) { $published = $null }   # nothing published yet (the site's own page)
	} catch {
		# Without it neither the seq nor (for the same version) the salt can be checked.
		throw "Couldn't read the published release-stable.txt ($($_.Exception.Message)). Try again when meowynetwork.com answers, or pass -Offline (the seq isn't checked then; the salt comes from -Salt, -NewSalt or -OutDir's document for this version)."
	}
}
if (-not $chosen -and -not $NewSalt -and $checked) {
	$chosen = SaltOf $published
	$from = "the published release-stable.txt"
}
if (-not $chosen -and -not $NewSalt -and -not $checked) {
	$local = Join-Path $OutDir "pack\v1\release-stable.txt"
	if (Test-Path $local) {
		$chosen = SaltOf ([System.IO.File]::ReadAllText($local))
		$from = $local
	}
}
if (-not $chosen) {
	# Not knowing whether this version is out already, a new salt could hand it to
	# a different set of installs: only when asked for.
	if (-not $checked -and -not $NewSalt) {
		throw "Offline, it can't tell whether $Version is already published (and with which salt). Pass -Salt <its salt> to sign it again, or -NewSalt for a new version."
	}
	$random = [byte[]]::new(8)
	[System.Security.Cryptography.RandomNumberGenerator]::Fill($random)
	$chosen = -join ($random | ForEach-Object { "{0:x2}" -f $_ })
	$from = "new"
}
Write-Host "Salt $chosen ($from)."
$lines += "salt $chosen"
$sources = @{ win64 = $Win64; win32 = $Win32 }
foreach ($arch in "win64", "win32") {
	$src = (Resolve-Path $sources[$arch]).Path
	$name = "gmcl_meowy_$arch.dll"
	Copy-Item $src (Join-Path $bin $name) -Force
	$size = (Get-Item $src).Length
	$hash = (Get-FileHash $src -Algorithm SHA256).Hash.ToLowerInvariant()
	$lines += "file $arch $size $hash /pack/v1/bin/$Version/$name"
}
# The seq: above the published document's. The same seq only for the very same
# version and files (signing it again for a later expiry or another rollout):
# both updaters may take a document at the floor's seq again when its file went
# missing, so two different documents must never share one.
if (-not $checked -and -not $Offline) {
	throw "Couldn't check -Seq against the published release-stable.txt. Try again when meowynetwork.com answers, or pass -Offline to sign without the check."
}
if ($published -and ($published -cmatch '(?m)^seq ([0-9]{1,19})$')) {
	$publishedSeq = [System.Numerics.BigInteger]::Parse($Matches[1])
	Write-Host "The published release document has seq $publishedSeq."
	if ($Seq -lt $publishedSeq) { throw "-Seq $Seq is below the published $($publishedSeq): every install would refuse it." }
	if ($Seq -eq $publishedSeq) {
		$same = ($published -cmatch "(?m)^version $([regex]::Escape($Version))$")
		foreach ($l in ($lines | Where-Object { $_.StartsWith("file ") })) {
			if (-not ($published -cmatch "(?m)^$([regex]::Escape($l))$")) { $same = $false }
		}
		if (-not $same) { throw "-Seq $Seq is the published document's, but this one names another version or other files: use -Seq $($publishedSeq + 1)." }
		Write-Host "Same seq, version and files as the published document: signing it again."
	}
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
