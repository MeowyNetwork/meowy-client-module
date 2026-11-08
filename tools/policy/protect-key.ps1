<#
.SYNOPSIS
Puts a password on an ops or recovery key file that has none (new-keys.ps1 made
them without one before it asked for passwords), in place.

.DESCRIPTION
Asks for a new password twice (at least 12 characters), writes the key again as
a password-protected PKCS#8 PEM (AES-256, PBKDF2-SHA256 at 600,000 rounds),
checks that the new file opens with that password and holds the same key, and
only then replaces the old file. sign-policy.ps1 asks for the password when it
signs. A file that is already protected is left alone.

Copies made before (backups, a USB stick, the FileSync key vault) still hold the
key without a password: back the protected file up again, then delete those.
Lose the password and the key is gone; the release key can still undo anything
the ops or recovery key signed.

.EXAMPLE
pwsh tools/policy/protect-key.ps1 -KeyFile $env:USERPROFILE\MeowyPackKeys\ops.key
#>
param(
	[Parameter(Mandatory)][string]$KeyFile,
	# For tests: the new password, instead of asking.
	[securestring]$Password
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "PolicyKeyFile.psm1") -Force

$path = (Resolve-Path $KeyFile).Path
$pem = [System.IO.File]::ReadAllText($path)
if (Test-PolicyKeyProtected $pem) {
	Write-Host "$path already has a password; nothing to do."
	return
}
$key = [System.Security.Cryptography.ECDsa]::Create()
try { $key.ImportFromPem($pem) } catch { throw "$path isn't a PEM private key." }
if ($key.KeySize -ne 256) { throw "$path isn't a P-256 key." }

$Password = Get-NewPolicyKeyPassword ([System.IO.Path]::GetFileName($path)) $Password
$tmp = "$path.protecting"
[System.IO.File]::WriteAllText($tmp, (Export-PolicyKey $key $Password))
try {
	$check = Import-PolicyKey $tmp $Password
	if (-not (Test-SamePolicyKey $key $check)) { throw "The protected copy doesn't hold the same key." }
} catch {
	Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
	throw "Nothing was changed: $($_.Exception.Message)"
}
[System.IO.File]::Replace($tmp, $path, [NullString]::Value)
Write-Host "$path now has a password. sign-policy.ps1 asks for it when it signs."
Write-Host "Back it up again (FileSync: Keys, Back up now), then delete any copy made before now: those still have no password."
