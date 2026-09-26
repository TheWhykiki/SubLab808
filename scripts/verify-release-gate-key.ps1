# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$KeyId,
    [Parameter(Mandatory)][string]$ExpectedPublicKeyXY
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'release-gate-key.ps1')
[void](Get-ReleaseGateSigningKey $KeyId $ExpectedPublicKeyXY)
Write-Output 'Versioned P-256 Key Vault identity verified.'
