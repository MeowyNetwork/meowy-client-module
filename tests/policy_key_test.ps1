# Password-protected ops/recovery key files: protect-key.ps1, sign-policy.ps1
# and PolicyKeyFile.psm1, on throwaway keys in a temp folder (never real ones).
# pwsh -NoProfile -File tests/policy_key_test.ps1
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Security.Cryptography
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root "tools\policy"
Import-Module (Join-Path $tools "PolicyKeyFile.psm1") -Force

$script:checks = 0
$script:failures = 0
function Check([string]$name, [bool]$ok) {
	$script:checks++
	if ($ok) { Write-Host "  ok    $name" } else { $script:failures++; Write-Host "  FAIL  $name" }
}
function Throws([scriptblock]$block, [string]$like) {
	try { & $block *> $null; return $false } catch { return $_.Exception.Message -like $like }
}
function Secure([string]$s) { return ConvertTo-SecureString $s -AsPlainText -Force }
function Hash([string]$p) { return (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash }
$sha = [System.Security.Cryptography.HashAlgorithmName]::SHA256

$dir = Join-Path ([System.IO.Path]::GetTempPath()) ("meowy-policy-key-test-" + [guid]::NewGuid().ToString("n"))
New-Item -ItemType Directory -Path $dir | Out-Null
try {
	$good = Secure "correct horse battery"
	$plainKey = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
	$keyFile = Join-Path $dir "ops.key"
	[System.IO.File]::WriteAllText($keyFile, $plainKey.ExportPkcs8PrivateKeyPem())

	# The policy installs keep, and a stricter one the ops key may sign (LF line ends).
	$kept = Join-Path $dir "kept.txt"
	[System.IO.File]::WriteAllText($kept, "meowy-pack-policy 1`nseq 1`npack_enabled 1`n")
	$policy = Join-Path $dir "policy.txt"
	[System.IO.File]::WriteAllText($policy, "meowy-pack-policy 1`nseq 2`npack_enabled 0`n")
	$sig = Join-Path $dir "policy.sig"
	function SigOk([System.Security.Cryptography.ECDsa]$k) {
		if (-not (Test-Path $sig)) { return $false }
		$hex = ([System.IO.File]::ReadAllText($sig)).Trim()
		$b = [byte[]]::new($hex.Length / 2)
		for ($i = 0; $i -lt $b.Length; $i++) { $b[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16) }
		return $k.VerifyData([System.IO.File]::ReadAllBytes($policy), $b, $sha)
	}

	Write-Host "== a key without a password still signs (with a warning)"
	& (Join-Path $tools "sign-policy.ps1") $policy -Key ops -KeyFile $keyFile -Kept $kept *> $null
	Check "signed with the unprotected key" (SigOk $plainKey)

	Write-Host "== protect-key.ps1"
	$before = Hash $keyFile
	Check "a password under 12 characters is refused" (Throws { & (Join-Path $tools "protect-key.ps1") -KeyFile $keyFile -Password (Secure "short") } "*at least 12*")
	Check "and the file is unchanged" ((Hash $keyFile) -eq $before)
	Check "and no half-written copy is left" (-not (Test-Path "$keyFile.protecting"))
	& (Join-Path $tools "protect-key.ps1") -KeyFile $keyFile -Password $good *> $null
	$pem = [System.IO.File]::ReadAllText($keyFile)
	Check "the file is now an encrypted PEM" ($pem.StartsWith("-----BEGIN ENCRYPTED PRIVATE KEY-----"))
	Check "with no unencrypted key in it" (-not $pem.Contains("-----BEGIN PRIVATE KEY-----"))
	Check "it opens with the password, as the same key" (Test-SamePolicyKey $plainKey (Import-PolicyKey $keyFile $good))
	Check "not with another password" (Throws { Import-PolicyKey $keyFile (Secure "wrong password here") } "*password*is wrong*")
	$protected = Hash $keyFile
	& (Join-Path $tools "protect-key.ps1") -KeyFile $keyFile -Password (Secure "another long password") *> $null
	Check "running it again leaves a protected file alone" ((Hash $keyFile) -eq $protected)
	Check "no half-written copy is left" (-not (Test-Path "$keyFile.protecting"))

	Write-Host "== sign-policy.ps1 with a protected key"
	Remove-Item $sig
	& (Join-Path $tools "sign-policy.ps1") $policy -Key ops -KeyFile $keyFile -Kept $kept -KeyPassword $good *> $null
	Check "signs with the right password" (SigOk $plainKey)
	Remove-Item $sig
	Check "refuses the wrong password" (Throws { & (Join-Path $tools "sign-policy.ps1") $policy -Key ops -KeyFile $keyFile -Kept $kept -KeyPassword (Secure "wrong password here") } "*password*is wrong*")
	Check "and writes no signature" (-not (Test-Path $sig))

	Write-Host "== PolicyKeyFile.psm1"
	$p384 = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP384)
	$other = Join-Path $dir "p384.key"
	[System.IO.File]::WriteAllText($other, $p384.ExportEncryptedPkcs8PrivateKeyPem("correct horse battery",
		[System.Security.Cryptography.PbeParameters]::new([System.Security.Cryptography.PbeEncryptionAlgorithm]::Aes256Cbc, $sha, 1000)))
	Check "a key that isn't P-256 is refused" (Throws { Import-PolicyKey $other $good } "*isn't a P-256 key*")
	$exported = Export-PolicyKey $plainKey $good
	Check "Export-PolicyKey writes AES-256 PBES2 (encrypted PEM)" ($exported.StartsWith("-----BEGIN ENCRYPTED PRIVATE KEY-----"))
	Check "Get-NewPolicyKeyPassword refuses a short given password" (Throws { Get-NewPolicyKeyPassword "x" (Secure "eleven char") } "*at least 12*")
	Check "and takes a long one" ((Get-NewPolicyKeyPassword "x" $good) -eq $good)
	Check "Test-SamePolicyKey tells two keys apart" (-not (Test-SamePolicyKey $plainKey ([System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256))))
} finally {
	Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "$script:checks checks, $script:failures failed"
if ($script:failures -ne 0) { exit 1 }
