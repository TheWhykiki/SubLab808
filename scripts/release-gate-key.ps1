# SPDX-License-Identifier: AGPL-3.0-only
# Azure CLI deliberately returns standard padded Base64, not the REST base64url wire shape.
function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw $Message }
}
function ConvertFrom-CanonicalBase64 {
    param([string] $Value, [int] $ExpectedLength)
    Assert-Condition ($Value.Length -le 512 -and $Value -cmatch '^[A-Za-z0-9+/]+={0,2}$') 'Invalid bounded Base64 value.'
    $bytes = [Convert]::FromBase64String($Value)
    Assert-Condition ($bytes.Length -eq $ExpectedLength -and [Convert]::ToBase64String($bytes) -ceq $Value) `
        'Noncanonical Base64 or unexpected byte length.'
    return ,$bytes
}

function Get-ReleaseGateSigningKey {
    param([string] $KeyId, [string] $ExpectedPublicKeyXY)
    Assert-Condition ($KeyId -cmatch '^https://[a-z0-9-]+\.vault\.azure\.net/keys/[A-Za-z0-9-]+/[a-f0-9]{32}$') `
        'Release-gate key must be an exact versioned Azure Key Vault key ID.'
    $raw = @(& az keyvault key show --id $KeyId --only-show-errors --output json)
    Assert-Condition ($LASTEXITCODE -eq 0) 'Cannot read release-gate Key Vault public key using OIDC credentials.'
    $json = $raw -join [Environment]::NewLine
    Assert-Condition ($json.Length -le 16384) 'Key Vault response exceeds the fixed bound.'
    $document = $json | ConvertFrom-Json
    Assert-Condition ($document.key.kid -ceq $KeyId -and $document.key.kty -ceq 'EC-HSM' -and
        $document.key.crv -ceq 'P-256' -and
        $document.attributes.enabled -is [bool] -and $document.attributes.enabled -eq $true -and
        $document.attributes.exportable -is [bool] -and $document.attributes.exportable -eq $false -and
        $document.key.keyOps -ccontains 'sign' -and $document.key.keyOps -cnotcontains 'export') `
        'Release-gate key must be enabled, non-exportable ECDSA P-256.'
    if ($document.PSObject.Properties.Name -contains 'releasePolicy') {
        Assert-Condition ($null -eq $document.releasePolicy) 'Exportable Key Vault release policies are forbidden.'
    }
    $x = ConvertFrom-CanonicalBase64 ([string]$document.key.x) 32
    $y = ConvertFrom-CanonicalBase64 ([string]$document.key.y) 32
    Assert-Condition ($x.Length -eq 32 -and $y.Length -eq 32 -and
        ([Convert]::ToHexString($x) + [Convert]::ToHexString($y)) -ceq $ExpectedPublicKeyXY) `
        'Key Vault public key does not match the configured active release-gate pin.'
    return [pscustomobject]@{ KeyId = $KeyId; PublicKeyXY = $ExpectedPublicKeyXY }
}

function New-LowSReleaseGateSignature {
    param(
        [object] $SigningKey,
        [byte[]] $Message
    )

    $digest = [Convert]::ToBase64String([System.Security.Cryptography.SHA256]::HashData($Message))
    $raw = @(& az keyvault key sign --id $SigningKey.KeyId --algorithm ES256 --digest $digest `
        --only-show-errors --output json)
    Assert-Condition ($LASTEXITCODE -eq 0) 'Key Vault failed to sign the release-gate authorization.'
    $json = $raw -join [Environment]::NewLine
    Assert-Condition ($json.Length -le 16384) 'Key Vault signature response exceeds the fixed bound.'
    $response = $json | ConvertFrom-Json
    Assert-Condition ($response.keyId -ceq $SigningKey.KeyId -and
        $response.algorithm -ceq 'ES256' -and
        [string]$response.signature -cmatch '^[A-Za-z0-9+/]{86}==$') 'Malformed Key Vault ES256 signature.'
    [byte[]] $signature = ConvertFrom-CanonicalBase64 ([string]$response.signature) 64
    $parameters = [System.Security.Cryptography.ECParameters]::new()
    $parameters.Curve = [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256
    $point = [System.Security.Cryptography.ECPoint]::new()
    $point.X = [Convert]::FromHexString($SigningKey.PublicKeyXY.Substring(0, 64))
    $point.Y = [Convert]::FromHexString($SigningKey.PublicKeyXY.Substring(64, 64))
    $parameters.Q = $point
    $verifier = [System.Security.Cryptography.ECDsa]::Create($parameters)
    try {
        Assert-Condition ($verifier.VerifyData($Message, $signature,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) `
            'Key Vault release-gate signature does not match the pinned public key.'
    } finally { $verifier.Dispose() }
    Assert-Condition ($signature.Length -eq 64) `
        'Release-gate signer did not return a P-256 P1363 signature.'
    [byte[]] $orderBytes = [Convert]::FromHexString(
        'FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551')
    [byte[]] $halfOrderBytes = [Convert]::FromHexString(
        '7FFFFFFF800000007FFFFFFFFFFFFFFFDE737D56D38BCF4279DCE5617E3192A8')
    [byte[]] $rBytes = [byte[]]::new(32)
    [byte[]] $sBytes = [byte[]]::new(32)
    [Array]::Copy($signature, 0, $rBytes, 0, 32)
    [Array]::Copy($signature, 32, $sBytes, 0, 32)
    try {
        $order = [System.Numerics.BigInteger]::new($orderBytes, $true, $true)
        $halfOrder = [System.Numerics.BigInteger]::new($halfOrderBytes, $true, $true)
        $r = [System.Numerics.BigInteger]::new($rBytes, $true, $true)
        $s = [System.Numerics.BigInteger]::new($sBytes, $true, $true)
        Assert-Condition ($r -gt [System.Numerics.BigInteger]::Zero -and $r -lt $order -and
                          $s -gt [System.Numerics.BigInteger]::Zero -and $s -lt $order) `
            'Release-gate signer returned an invalid P-256 scalar.'
        if ($s -gt $halfOrder) {
            [byte[]] $lowS = ($order - $s).ToByteArray($true, $true)
            try {
                Assert-Condition ($lowS.Length -gt 0 -and $lowS.Length -le 32) `
                    'Release-gate low-S normalization failed.'
                [Array]::Clear($signature, 32, 32)
                [Array]::Copy($lowS, 0, $signature, 64 - $lowS.Length, $lowS.Length)
            } finally {
                [Array]::Clear($lowS, 0, $lowS.Length)
            }
        }
        return [Convert]::ToHexString($signature)
    } finally {
        [Array]::Clear($signature, 0, $signature.Length)
        [Array]::Clear($orderBytes, 0, $orderBytes.Length)
        [Array]::Clear($halfOrderBytes, 0, $halfOrderBytes.Length)
        [Array]::Clear($rBytes, 0, $rBytes.Length)
        [Array]::Clear($sBytes, 0, $sBytes.Length)
    }
}
