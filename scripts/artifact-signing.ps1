# SPDX-License-Identifier: AGPL-3.0-only
# Product-neutral Artifact Signing Public Trust contract. No private key is imported.
function Test-ArtifactSigningProfileEku {
    param([string] $Value)
    if ($Value -cnotmatch '^1\.3\.6\.1\.4\.1\.311\.97\.((0|[1-9][0-9]{0,9})\.){3}(0|[1-9][0-9]{0,9})$' -or
        $Value.StartsWith('1.3.6.1.4.1.311.97.1.')) { return $false }
    foreach ($part in $Value.Substring('1.3.6.1.4.1.311.97.'.Length).Split('.')) {
        if ([uint64]$part -gt [uint32]::MaxValue) { return $false }
    }
    return $true
}

function Assert-ArtifactSigningIdentity {
    param([string] $Path, [string] $ExpectedProfileEku)
    if (-not (Test-ArtifactSigningProfileEku $ExpectedProfileEku)) { throw 'Invalid Artifact Signing identity EKU.' }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or $null -eq $signature.TimeStamperCertificate) {
        throw "Valid Authenticode signature and timestamp are required: $Path"
    }
    $extensions = @($signature.SignerCertificate.Extensions | Where-Object { $_.Oid.Value -ceq '2.5.29.37' })
    if ($extensions.Count -ne 1) { throw 'A unique signer EKU extension is required.' }
    $eku = [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new(
        $extensions[0], $extensions[0].Critical)
    $identifiers = @($eku.EnhancedKeyUsages | ForEach-Object { $_.Value })
    foreach ($required in @('1.3.6.1.5.5.7.3.3', '1.3.6.1.4.1.311.97.1.0', $ExpectedProfileEku)) {
        if (@($identifiers | Where-Object { $_ -ceq $required }).Count -ne 1) {
            throw "Signed file is outside the pinned Artifact Signing Public Trust identity: $Path"
        }
    }
    return [pscustomobject]@{
        ProfileEku = $ExpectedProfileEku
        SignerSha256 = $signature.SignerCertificate.GetCertHashString(
            [System.Security.Cryptography.HashAlgorithmName]::SHA256).ToUpperInvariant()
        TimestampCertificateSha256 = $signature.TimeStamperCertificate.GetCertHashString(
            [System.Security.Cryptography.HashAlgorithmName]::SHA256).ToUpperInvariant()
    }
}
