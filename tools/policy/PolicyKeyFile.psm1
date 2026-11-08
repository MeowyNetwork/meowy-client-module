<#
The pack policy's ops and recovery key files (src/policy.h): password-protected
PKCS#8 PEM ("ENCRYPTED PRIVATE KEY", AES-256-CBC, PBKDF2-SHA256 at 600,000
rounds), read by sign-policy.ps1 and written by new-keys.ps1 and protect-key.ps1.

An old unprotected file ("PRIVATE KEY") still signs, with a warning saying how
to protect it. Passwords are asked for with Read-Host -AsSecureString unless a
SecureString is passed in, and are never written anywhere.
#>
Set-StrictMode -Version 3
Add-Type -AssemblyName System.Security.Cryptography

$script:MinPasswordLength = 12
$script:Pbe = [System.Security.Cryptography.PbeParameters]::new(
	[System.Security.Cryptography.PbeEncryptionAlgorithm]::Aes256Cbc,
	[System.Security.Cryptography.HashAlgorithmName]::SHA256,
	600000)

function Test-PolicyKeyProtected([string]$Pem) {
	return $Pem.Contains("-----BEGIN ENCRYPTED PRIVATE KEY-----")
}

function ConvertFrom-KeyPassword([securestring]$Password) {
	return [System.Net.NetworkCredential]::new("", $Password).Password
}

# A new password, asked twice, at least 12 characters (or the one passed in, checked the same way).
function Get-NewPolicyKeyPassword([string]$What, [securestring]$Given) {
	if ($Given) {
		if ((ConvertFrom-KeyPassword $Given).Length -lt $script:MinPasswordLength) {
			throw "The password for $What must be at least $script:MinPasswordLength characters."
		}
		return $Given
	}
	while ($true) {
		$a = Read-Host -AsSecureString "New password for $What (at least $script:MinPasswordLength characters)"
		$b = Read-Host -AsSecureString "The same password again"
		$pa = ConvertFrom-KeyPassword $a
		if ($pa -cne (ConvertFrom-KeyPassword $b)) { Write-Warning "The two passwords didn't match. Try again."; continue }
		if ($pa.Length -lt $script:MinPasswordLength) { Write-Warning "That's shorter than $script:MinPasswordLength characters. Try again."; continue }
		return $a
	}
}

# The key in a key file: asks for its password when it's protected.
function Import-PolicyKey([string]$Path, [securestring]$Password) {
	$pem = [System.IO.File]::ReadAllText($Path)
	$key = [System.Security.Cryptography.ECDsa]::Create()
	if (Test-PolicyKeyProtected $pem) {
		if (-not $Password) { $Password = Read-Host -AsSecureString "Password for $([System.IO.Path]::GetFileName($Path))" }
		try {
			$key.ImportFromEncryptedPem($pem, (ConvertFrom-KeyPassword $Password))
		} catch {
			throw "The password for $Path is wrong (or the file is damaged)."
		}
	} else {
		Write-Warning "$Path isn't password-protected. Protect it with: pwsh tools/policy/protect-key.ps1 -KeyFile `"$Path`""
		$key.ImportFromPem($pem)
	}
	if ($key.KeySize -ne 256) { throw "$Path isn't a P-256 key." }
	return $key
}

# The key as a password-protected PEM.
function Export-PolicyKey([System.Security.Cryptography.ECDsa]$Key, [securestring]$Password) {
	return $Key.ExportEncryptedPkcs8PrivateKeyPem((ConvertFrom-KeyPassword $Password), $script:Pbe)
}

# Whether two keys are the same key (their public halves match).
function Test-SamePolicyKey([System.Security.Cryptography.ECDsa]$A, [System.Security.Cryptography.ECDsa]$B) {
	$qa = $A.ExportParameters($false).Q
	$qb = $B.ExportParameters($false).Q
	return [System.Linq.Enumerable]::SequenceEqual([byte[]]$qa.X, [byte[]]$qb.X) -and
		[System.Linq.Enumerable]::SequenceEqual([byte[]]$qa.Y, [byte[]]$qb.Y)
}

Export-ModuleMember -Function Test-PolicyKeyProtected, Get-NewPolicyKeyPassword, Import-PolicyKey, Export-PolicyKey, Test-SamePolicyKey
