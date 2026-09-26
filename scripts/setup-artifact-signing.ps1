# SPDX-License-Identifier: AGPL-3.0-only
# Install the exact Microsoft client/runtime used for both signing architectures.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Endpoint,
    [Parameter(Mandatory)][string]$AccountName,
    [Parameter(Mandatory)][string]$ProfileName
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Endpoint -cnotmatch '^https://[a-z0-9]+\.codesigning\.azure\.net/?$' -or
    $AccountName -cnotmatch '^[A-Za-z][A-Za-z0-9-]{2,99}$' -or
    $ProfileName -cnotmatch '^[A-Za-z][A-Za-z0-9-]{3,98}[A-Za-z0-9]$') {
    throw 'Artifact Signing account/profile configuration is missing or malformed.'
}
if (-not $env:RUNNER_TEMP -or -not $env:GITHUB_ENV) { throw 'This setup requires an ephemeral Actions runner.' }
$directory = Join-Path $env:RUNNER_TEMP ("whykiki-artifact-signing-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory -ErrorAction Stop | Out-Null
$client = Join-Path $directory 'client.zip'
Invoke-WebRequest -Uri 'https://api.nuget.org/v3-flatcontainer/microsoft.artifactsigning.client/1.0.128/microsoft.artifactsigning.client.1.0.128.nupkg' -OutFile $client
if ((Get-FileHash -LiteralPath $client -Algorithm SHA256).Hash -cne
    '74BD7D27E6CE1051409C38D9B46BC8DF0400ECD643D51FFBF2AC00869061E40B') {
    throw 'Microsoft Artifact Signing client package digest mismatch.'
}
Expand-Archive -LiteralPath $client -DestinationPath (Join-Path $directory 'client') -ErrorAction Stop
$dlib = Join-Path $directory 'client/bin/x64/Azure.CodeSigning.Dlib.dll'
$dlibHash = '2D4C1BBC87467B3AC25BBC49DF58CC8B36A0F92B3E21AA98BBBAD08A4D7C98BA'
if ((Get-FileHash -LiteralPath $dlib -Algorithm SHA256).Hash -cne $dlibHash) {
    throw 'Artifact Signing x64 dlib digest mismatch.'
}
# Signing uses an x64 process, including on Windows on Arm. Product build and
# native host validation still run on the target architecture.
$runtime = Join-Path $directory 'runtime.zip'
Invoke-WebRequest -Uri 'https://builds.dotnet.microsoft.com/dotnet/Runtime/8.0.31/dotnet-runtime-8.0.31-win-x64.zip' -OutFile $runtime
if ((Get-FileHash -LiteralPath $runtime -Algorithm SHA512).Hash -cne
    '9C55C58694676EE64B0EED2CD6D8CBF58B9AA8288420ACC66841E15CA0099C75D4AF0182D23A641C2342E5A151A325DF4A12FA0BDE2E47C0FB7E9A33E7B09896') {
    throw 'Microsoft .NET x64 runtime digest mismatch.'
}
$runtimeDirectory = Join-Path $directory 'dotnet-x64'
Expand-Archive -LiteralPath $runtime -DestinationPath $runtimeDirectory -ErrorAction Stop
& (Join-Path $runtimeDirectory 'dotnet.exe') --list-runtimes
if ($LASTEXITCODE -ne 0) { throw 'The pinned x64 .NET runtime cannot execute on this runner.' }
$metadata = Join-Path $directory 'metadata.json'
@{
    Endpoint = $Endpoint
    CodeSigningAccountName = $AccountName
    CertificateProfileName = $ProfileName
    ExcludeCredentials = @(
        'EnvironmentCredential', 'WorkloadIdentityCredential', 'ManagedIdentityCredential',
        'SharedTokenCacheCredential', 'VisualStudioCredential', 'VisualStudioCodeCredential',
        'AzurePowerShellCredential', 'AzureDeveloperCliCredential', 'InteractiveBrowserCredential'
    )
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $metadata -Encoding utf8
@(
    "WK_SIGNING_DLIB_PATH=$dlib",
    "WK_SIGNING_DLIB_SHA256=$dlibHash",
    "WK_SIGNING_METADATA_PATH=$metadata",
    "DOTNET_ROOT_X64=$runtimeDirectory"
) | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
