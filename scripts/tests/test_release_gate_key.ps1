# SPDX-License-Identifier: AGPL-3.0-only
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../release-gate-key.ps1')
. (Join-Path $PSScriptRoot '../artifact-signing.ps1')
$script:testKey = [System.Security.Cryptography.ECDsa]::Create(
    [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
$public = $script:testKey.ExportParameters($false)
$expected = [Convert]::ToHexString($public.Q.X) + [Convert]::ToHexString($public.Q.Y)
$script:keyId = 'https://example.vault.azure.net/keys/release/0123456789abcdef0123456789abcdef'
$script:record = @{
    key = @{ kid = $script:keyId; kty = 'EC-HSM'; crv = 'P-256'; keyOps = @('sign', 'verify')
        x = [Convert]::ToBase64String($public.Q.X); y = [Convert]::ToBase64String($public.Q.Y) }
    attributes = @{ enabled = $true; exportable = $false }
    releasePolicy = $null
}
$script:signMutation = ''
function az {
    $global:LASTEXITCODE = 0
    if ($args[2] -ceq 'show') {
        return (($script:record | ConvertTo-Json -Depth 8) -split "\r?\n")
    }
    if ($args[2] -cne 'sign') { throw 'Unexpected mocked Azure command.' }
    $digestIndex = [Array]::IndexOf($args, '--digest')
    $digest = [Convert]::FromBase64String($args[$digestIndex + 1])
    $signature = $script:testKey.SignHash($digest,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    $response = @{ keyId = $script:keyId; algorithm = 'ES256'; signature = [Convert]::ToBase64String($signature) }
    switch ($script:signMutation) {
        'key' { $response.keyId += '0' }
        'algorithm' { $response.algorithm = 'ES384' }
        'signature' { $response.signature = [Convert]::ToBase64String([byte[]]::new(64)) }
        'padding' { $response.signature = $response.signature.TrimEnd('=') }
    }
    return (($response | ConvertTo-Json) -split "\r?\n")
}
function Assert-Rejected([scriptblock] $Action, [string] $Description) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw "Accepted negative case: $Description" }
}
try {
    $contract = Get-ReleaseGateSigningKey $script:keyId $expected
    if ($contract.PublicKeyXY -cne $expected) { throw 'Public pin mismatch.' }
    [byte[]]$message = [System.Text.Encoding]::UTF8.GetBytes('fresh, bound release authorization')
    $signatureHex = New-LowSReleaseGateSignature $contract $message
    $signature = [Convert]::FromHexString($signatureHex)
    if (-not $script:testKey.VerifyData($message, $signature,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) {
        throw 'Signature did not survive low-S normalization.'
    }
    $s = [System.Numerics.BigInteger]::new([byte[]]$signature[32..63], $true, $true)
    $half = [System.Numerics.BigInteger]::new([Convert]::FromHexString(
        '7FFFFFFF800000007FFFFFFFFFFFFFFFDE737D56D38BCF4279DCE5617E3192A8'), $true, $true)
    if ($s -gt $half) { throw 'Signature is not low-S.' }
    $original = $script:record | ConvertTo-Json -Depth 8
    foreach ($mutation in @('disabled', 'exportable', 'missing-exportable', 'software', 'curve',
                           'operations', 'release-policy', 'key-id', 'coordinate')) {
        $script:record = $original | ConvertFrom-Json -AsHashtable
        switch ($mutation) {
            'disabled' { $script:record.attributes.enabled = $false }
            'exportable' { $script:record.attributes.exportable = $true }
            'missing-exportable' { $script:record.attributes.Remove('exportable') }
            'software' { $script:record.key.kty = 'EC' }
            'curve' { $script:record.key.crv = 'P-384' }
            'operations' { $script:record.key.keyOps = @('verify') }
            'release-policy' { $script:record.releasePolicy = @{ any = 'export' } }
            'key-id' { $script:record.key.kid += '0' }
            'coordinate' { $script:record.key.x = $script:record.key.x.TrimEnd('=') }
        }
        Assert-Rejected { Get-ReleaseGateSigningKey $script:keyId $expected } $mutation
    }
    $script:record = $original | ConvertFrom-Json -AsHashtable
    Assert-Rejected { Get-ReleaseGateSigningKey $script:keyId ('0' * 128) } 'wrong public pin'
    Assert-Rejected { Get-ReleaseGateSigningKey 'https://example.vault.azure.net/keys/release' $expected } 'unversioned key'
    foreach ($mutation in @('key', 'algorithm', 'signature', 'padding')) {
        $script:signMutation = $mutation
        Assert-Rejected { New-LowSReleaseGateSignature $contract $message } "signature $mutation"
    }
    if (-not (Test-ArtifactSigningProfileEku '1.3.6.1.4.1.311.97.100.200.300.400')) { throw 'Valid identity OID rejected.' }
    foreach ($oid in @('', '1.3.6.1.4.1.311.97.1.0', '1.3.6.1.4.1.311.97.1.3.1.44',
        '1.3.6.1.4.1.311.97.100.200.300.400.', '1.3.6.1.4.1.311.97.4294967296.200.300.400')) {
        if (Test-ArtifactSigningProfileEku $oid) { throw "Malformed identity OID accepted: $oid" }
    }
    Write-Output 'PASS: Key Vault CLI encoding, non-exportable HSM gate and locally verified low-S signatures'
} finally { $script:testKey.Dispose() }
