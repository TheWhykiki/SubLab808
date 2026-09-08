#requires -Version 7.2

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('bootstrap', 'upgrade')]
    [string] $Mode,

    [Parameter(Mandatory = $true)]
    [string] $Product,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64ec')]
    [string] $Architecture,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64')]
    [string] $ExpectedMsiArchitecture,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedManufacturer,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedUpgradeCode,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedOtherArchitectureUpgradeCode,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $CandidateReleaseId,

    [Parameter(Mandatory = $true)]
    [string] $CandidateTag,

    [Parameter(Mandatory = $true)]
    [string] $CandidateSourceCommit,

    [ValidateRange(0, [long]::MaxValue)]
    [long] $BaselineReleaseId = 0,

    [string] $BaselineTag,
    [string] $BaselineVersion,
    [string] $BaselineSourceCommit,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedSignerSha256,

    [string] $ExpectedNextSignerSha256,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedReleaseGatePublicKeyXY,

    [string] $ExpectedReleaseGateNextPublicKeyXY,

    [Parameter(Mandatory = $true)]
    [string] $ReleaseGatePrivateKeyPkcs8Base64,

    [Parameter(Mandatory = $true)]
    [string] $HostTestPath,

    [Parameter(Mandatory = $true)]
    [string] $CleanInstallGatePath,

    [Parameter(Mandatory = $true)]
    [string] $ReceiptPath,

    [ValidateRange(1, 3600)]
    [int] $InstallerTimeoutSeconds = 600,

    [ValidateRange(1, 3600)]
    [int] $UpdaterTimeoutSeconds = 3600,

    [ValidateRange(1, 3600)]
    [int] $HostTestTimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:InstallStateUnknown = -1
$script:InstallStateDefault = 5
$script:Owner = 'TheWhykiki'
$script:ReleaseGateSchema = 'whykiki.windows-updater-release-gate'
$script:TransitionSchema = 'whykiki.windows-updater-transition-acceptance'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw $Message }
}

function Resolve-SafeFile {
    param([string] $Path, [string] $Description)

    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Path)) "$Description path is required."
    $full = [System.IO.Path]::GetFullPath($Path)
    Assert-Condition (Test-Path -LiteralPath $full -PathType Leaf) "$Description was not found: $full"
    $item = Get-Item -LiteralPath $full -Force
    Assert-Condition (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Description must not be a reparse point: $full"
    return $full
}

function Normalize-Guid {
    param([string] $Value, [string] $Description)

    Assert-Condition ($Value -cmatch `
        '^\{?[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}\}?$') `
        "$Description is not a valid GUID."
    return ([guid]$Value).ToString('D').ToUpperInvariant()
}

function ConvertTo-VersionTuple {
    param([string] $Version, [string] $Description)

    Assert-Condition ($Version -cmatch `
        '^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$') `
        "$Description is not a canonical MSI version."
    $parts = @($Version.Split('.') | ForEach-Object { [int]$_ })
    Assert-Condition ($parts[0] -le 255 -and $parts[1] -le 255 -and $parts[2] -le 65535) `
        "$Description exceeds Windows Installer bounds."
    return ,$parts
}

function Compare-VersionTuple {
    param([int[]] $Left, [int[]] $Right)
    for ($index = 0; $index -lt 3; ++$index) {
        if ($Left[$index] -lt $Right[$index]) { return -1 }
        if ($Left[$index] -gt $Right[$index]) { return 1 }
    }
    return 0
}

function Get-PublicRelease {
    param([long] $ReleaseId)

    $uri = "https://api.github.com/repos/$script:Owner/$Product/releases/$ReleaseId"
    $headers = @{
        Accept = 'application/vnd.github+json'
        'X-GitHub-Api-Version' = '2026-03-10'
    }
    return Invoke-RestMethod -Method Get -Uri $uri -Headers $headers `
        -UserAgent 'Whykiki-Windows-Release-Transition-Gate/1'
}

function Get-ExpectedReleaseAssetNames {
    param([string] $Version)

    return @(
        "$Product-$Version-Windows-x64.msi"
        "$Product-$Version-Windows-x64.evidence.json"
        "$Product-$Version-Windows-arm64ec.msi"
        "$Product-$Version-Windows-arm64ec.evidence.json"
        "$Product-$Version-macOS-universal.pkg"
        "$Product-$Version-macOS-universal-VST3.zip"
        "$Product-$Version-macOS-universal.evidence.json"
        "$Product-$Version-SHA256SUMS.txt"
    )
}

function Assert-ReleaseContract {
    param(
        [object] $Release,
        [long] $ReleaseId,
        [string] $Tag,
        [string] $SourceCommit,
        [bool] $ExpectedPrerelease,
        [string] $Version
    )

    Assert-Condition ($null -ne $Release -and $Release -is [psobject]) `
        'GitHub release metadata is not an object.'
    Assert-Condition ([long]$Release.id -eq $ReleaseId) 'GitHub release ID mismatch.'
    Assert-Condition ([string]$Release.tag_name -ceq $Tag) 'GitHub release tag mismatch.'
    Assert-Condition ([string]$Release.target_commitish -ceq $SourceCommit) `
        'GitHub release target commit mismatch.'
    Assert-Condition ($Release.draft -is [bool] -and -not $Release.draft) `
        'Release transition accepts no draft.'
    Assert-Condition ($Release.prerelease -is [bool] -and
                      $Release.prerelease -eq $ExpectedPrerelease) `
        'GitHub release prerelease state mismatch.'
    Assert-Condition ($Release.immutable -is [bool] -and $Release.immutable) `
        'GitHub release is not immutable.'
    $expectedReleaseUrl = "https://github.com/$script:Owner/$Product/releases/tag/$Tag"
    Assert-Condition ([string]$Release.html_url -ceq $expectedReleaseUrl) `
        'GitHub release URL is not canonical.'

    $assets = @($Release.assets)
    $expectedNames = @(Get-ExpectedReleaseAssetNames $Version)
    Assert-Condition ($assets.Count -eq $expectedNames.Count) `
        'Release does not contain exactly eight cross-platform assets.'
    $byName = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::Ordinal)
    foreach ($asset in $assets) {
        $name = [string]$asset.name
        Assert-Condition ($name -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,199}$') `
            'Release contains an unsafe asset name.'
        Assert-Condition ($byName.TryAdd($name, $asset)) `
            "Release contains duplicate asset name: $name"
        Assert-Condition ([string]$asset.state -ceq 'uploaded') `
            "Release asset is not completely uploaded: $name"
        Assert-Condition ([string]$asset.digest -cmatch '^sha256:[0-9a-f]{64}$') `
            "Release asset has no canonical server SHA-256: $name"
        Assert-Condition ([long]$asset.id -gt 0 -and [long]$asset.size -gt 0) `
            "Release asset ID or size is invalid: $name"
        $expectedUrl = "https://github.com/$script:Owner/$Product/releases/download/$Tag/$name"
        Assert-Condition ([string]$asset.browser_download_url -ceq $expectedUrl) `
            "Release asset URL is not canonical: $name"
    }
    foreach ($name in $expectedNames) {
        Assert-Condition ($byName.ContainsKey($name)) "Required release asset is missing: $name"
    }
    return ,$byName
}

function Receive-VerifiedAsset {
    param([object] $Asset, [string] $Destination, [long] $MaximumBytes)

    $destinationFull = [System.IO.Path]::GetFullPath($Destination)
    Assert-Condition ($MaximumBytes -gt 0 -and [long]$Asset.size -gt 0 -and
                      [long]$Asset.size -le $MaximumBytes) `
        "Release asset exceeds its download size bound: $($Asset.name)"
    Assert-Condition (-not (Test-Path -LiteralPath $destinationFull)) `
        "Download destination already exists: $destinationFull"
    Invoke-WebRequest -Method Get -Uri ([string]$Asset.browser_download_url) `
        -OutFile $destinationFull -TimeoutSec 600 `
        -UserAgent 'Whykiki-Windows-Release-Transition-Gate/1'
    $resolved = Resolve-SafeFile $destinationFull 'Downloaded release asset'
    $item = Get-Item -LiteralPath $resolved -Force
    Assert-Condition ([long]$item.Length -eq [long]$Asset.size) `
        "Downloaded asset size mismatch: $($Asset.name)"
    $digest = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-Condition ("sha256:$digest" -ceq [string]$Asset.digest) `
        "Downloaded asset digest mismatch: $($Asset.name)"
    return $resolved
}

function Assert-NoAlternateDataStreams {
    param([string] $Path, [string] $DisplayPath)

    $streams = @(Get-Item -LiteralPath $Path -Stream '*' -ErrorAction Stop)
    $unexpected = @($streams | Where-Object { $_.Stream -notin @(':$DATA', '$DATA') })
    Assert-Condition ($unexpected.Count -eq 0) `
        "Alternate data streams are forbidden in the installed payload: $DisplayPath"
}

function Get-ExpectedPayloadContract {
    param([object[]] $PayloadFiles)

    Assert-Condition ($PayloadFiles.Count -gt 0) 'Evidence contains no payload files.'
    $files = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::Ordinal)
    $directories = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::Ordinal)
    $pathKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $PayloadFiles) {
        $properties = @($entry.PSObject.Properties.Name | Sort-Object)
        Assert-Condition (($properties -join '|') -ceq 'path|sha256|size') `
            'Each payload evidence row must contain exactly path, sha256 and size.'
        $relative = [string]$entry.path
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($relative) -and
                          $relative -cnotmatch '/' -and $relative -cnotmatch ':' -and
                          -not [System.IO.Path]::IsPathRooted($relative)) `
            "Payload evidence contains an unsafe path: $relative"
        $parts = @($relative.Split([char]'\'))
        Assert-Condition ($parts.Count -ge 2 -and $parts[0] -ceq 'Contents') `
            "Payload evidence path must be below Contents: $relative"
        foreach ($part in $parts) {
            Assert-Condition (-not [string]::IsNullOrWhiteSpace($part) -and
                              $part -cne '.' -and $part -cne '..' -and
                              $part -cnotmatch '[\x00-\x1F<>:"/\\|?*]' -and
                              $part -cnotmatch '[ .]$' -and
                              $part -cnotmatch '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') `
                "Payload evidence contains an unsafe path component: $relative"
        }
        Assert-Condition ($pathKeys.Add($relative)) `
            "Payload evidence contains a duplicate or case-colliding path: $relative"
        $sha256 = [string]$entry.sha256
        Assert-Condition ($sha256 -cmatch '^[0-9A-F]{64}$') `
            "Payload evidence contains a non-canonical SHA-256: $relative"
        $length = [long]$entry.size
        Assert-Condition ($length -ge 0 -and ([string]$length -ceq ([string]$entry.size)) `
            "Payload evidence contains a non-canonical size: $relative"
        $files.Add($relative, [pscustomobject]@{
            RelativePath = $relative
            Length = $length
            Sha256 = $sha256
        })
        for ($index = 0; $index -lt $parts.Count - 1; ++$index) {
            [void]$directories.Add(($parts[0..$index] -join '\'))
        }
    }
    return [pscustomobject]@{ Directories = $directories; Files = $files }
}

function Get-SafeTreeSnapshot {
    param([string] $Root)

    $rootFull = [System.IO.Path]::GetFullPath($Root)
    Assert-Condition (Test-Path -LiteralPath $rootFull -PathType Container) `
        "Installed VST3 bundle was not found: $rootFull"
    $rootItem = Get-Item -LiteralPath $rootFull -Force
    Assert-Condition (($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
        "Installed VST3 root must not be a reparse point: $rootFull"
    Assert-NoAlternateDataStreams $rootFull '<bundle-root>'
    $pending = [System.Collections.Generic.Queue[System.IO.DirectoryInfo]]::new()
    $pending.Enqueue($rootItem)
    $directories = [System.Collections.Generic.List[string]]::new()
    $files = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::Ordinal)
    $pathKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($entry in $directory.EnumerateFileSystemInfos()) {
            $relative = [System.IO.Path]::GetRelativePath($rootFull, $entry.FullName).Replace('/', '\')
            Assert-Condition (-not $relative.StartsWith('..')) `
                "Installed payload entry escaped its root: $($entry.FullName)"
            Assert-Condition (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
                "Reparse points are forbidden in the installed payload: $relative"
            Assert-Condition ($pathKeys.Add($relative)) "Case-colliding installed path: $relative"
            Assert-NoAlternateDataStreams $entry.FullName $relative
            if ($entry -is [System.IO.DirectoryInfo]) {
                $directories.Add($relative)
                $pending.Enqueue($entry)
            } else {
                Assert-Condition ($entry -is [System.IO.FileInfo]) `
                    "Unsupported filesystem entry in installed payload: $relative"
                $files.Add($relative, [pscustomobject]@{
                    RelativePath = $relative
                    FullName = $entry.FullName
                    Length = [long]$entry.Length
                    Sha256 = (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
                })
            }
        }
    }
    return [pscustomobject]@{
        Root = $rootFull
        Directories = $directories.ToArray()
        Files = $files
    }
}

function Assert-InstalledPayloadMatchesEvidence {
    param([string] $InstalledBundle, [object[]] $PayloadFiles)

    $expected = Get-ExpectedPayloadContract $PayloadFiles
    $actual = Get-SafeTreeSnapshot $InstalledBundle
    Assert-Condition ($actual.Directories.Count -eq $expected.Directories.Count) `
        'Installed payload directory count differs from signed evidence.'
    foreach ($directory in $actual.Directories) {
        Assert-Condition ($expected.Directories.Contains($directory)) `
            "Unexpected installed payload directory: $directory"
    }
    Assert-Condition ($actual.Files.Count -eq $expected.Files.Count) `
        'Installed payload file count differs from signed evidence.'
    foreach ($relative in $expected.Files.Keys) {
        Assert-Condition ($actual.Files.ContainsKey($relative)) `
            "Installed payload file is missing or has different casing: $relative"
        $expectedFile = $expected.Files[$relative]
        $actualFile = $actual.Files[$relative]
        Assert-Condition ($actualFile.Length -eq $expectedFile.Length -and
                          $actualFile.Sha256 -ceq $expectedFile.Sha256) `
            "Installed payload bytes differ from signed evidence: $relative"
    }
}

function Read-EvidenceContract {
    param(
        [string] $Path,
        [string] $Version,
        [string] $SourceCommit,
        [string] $ExpectedMsiFile,
        [string] $ExpectedCurrentPin,
        [string] $ExpectedNextPin,
        [string] $ExpectedCurrentReleaseGatePublicKeyXY,
        [string] $ExpectedNextReleaseGatePublicKeyXY,
        [bool] $IsCandidate
    )

    $raw = Get-Content -LiteralPath $Path -Raw -Encoding utf8
    Assert-Condition ($raw.Length -gt 0 -and $raw.Length -le 8MB) `
        'Windows evidence has an invalid size.'
    $evidence = $raw | ConvertFrom-Json
    Assert-Condition ($evidence.schemaVersion -eq 4) 'Unsupported Windows evidence schema.'
    Assert-Condition ([string]$evidence.artifactStatus -ceq 'SIGNED' -and
                      $evidence.signed -is [bool] -and $evidence.signed) `
        'Windows transition requires signed production evidence.'
    Assert-Condition ([string]$evidence.product -ceq $Product) 'Evidence product mismatch.'
    Assert-Condition ([string]$evidence.version -ceq $Version) 'Evidence version mismatch.'
    Assert-Condition ([string]$evidence.sourceCommit -ceq $SourceCommit) `
        'Evidence source commit mismatch.'
    Assert-Condition ([string]$evidence.payloadArchitecture -ceq $Architecture) `
        'Evidence payload architecture mismatch.'
    Assert-Condition ([string]$evidence.msiArchitecture -ceq $ExpectedMsiArchitecture) `
        'Evidence MSI architecture mismatch.'
    Assert-Condition ([string]$evidence.upgradeCode -ceq $normalizedUpgradeCode -and
                      [string]$evidence.otherArchitectureUpgradeCode -ceq $normalizedOtherUpgradeCode) `
        'Evidence UpgradeCodes mismatch.'
    Assert-Condition ([string]$evidence.validation.manufacturer -ceq $ExpectedManufacturer) `
        'Evidence manufacturer mismatch.'
    Assert-Condition ([string]$evidence.msiFile -ceq $ExpectedMsiFile) 'Evidence MSI filename mismatch.'
    Assert-Condition ([string]$evidence.msiSha256 -cmatch '^[0-9A-F]{64}$') `
        'Evidence MSI SHA-256 is not canonical.'
    Assert-Condition ([string]$evidence.productCode -cmatch `
        '^[0-9A-F]{8}-[0-9A-F]{4}-[1-5][0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}$') `
        'Evidence ProductCode is not canonical.'

    $signer = ([string]$evidence.signerCertificateSha256).Replace(' ', '').ToUpperInvariant()
    $updaterCurrent = ([string]$evidence.updaterCurrentSignerSha256).Replace(' ', '').ToUpperInvariant()
    $updaterNext = ([string]$evidence.updaterNextSignerSha256).Replace(' ', '').ToUpperInvariant()
    Assert-Condition ($signer -cmatch '^[0-9A-F]{64}$' -and $updaterCurrent -ceq $signer) `
        'Evidence current signer identity is malformed or inconsistent.'
    Assert-Condition ([string]::IsNullOrEmpty($updaterNext) -or
                      ($updaterNext -cmatch '^[0-9A-F]{64}$' -and $updaterNext -cne $signer)) `
        'Evidence next signer identity is malformed or duplicates current.'
    [string[]]$allowlist = @($evidence.payloadSignerAllowlistSha256)
    $expectedAllowlist = if ([string]::IsNullOrEmpty($updaterNext)) {
        @($signer)
    } else {
        @($signer, $updaterNext)
    }
    Assert-Condition ($allowlist.Count -eq $expectedAllowlist.Count) `
        'Evidence signer allowlist has the wrong size.'
    for ($index = 0; $index -lt $expectedAllowlist.Count; ++$index) {
        Assert-Condition ($allowlist[$index] -ceq $expectedAllowlist[$index]) `
            'Evidence signer allowlist is not canonical.'
    }
    if ($IsCandidate) {
        Assert-Condition ($signer -ceq $ExpectedCurrentPin -and
                          $updaterNext -ceq $ExpectedNextPin) `
            'Candidate evidence does not match the configured current/next pins.'
    }
    $releaseGateCurrent = [string]$evidence.releaseGatePublicKeyXY
    $releaseGateNext = [string]$evidence.releaseGateNextPublicKeyXY
    Assert-Condition ($releaseGateCurrent -cmatch '^[0-9A-F]{128}$') `
        'Evidence active release-gate public key is not canonical P-256 X||Y.'
    Assert-Condition ([string]::IsNullOrEmpty($releaseGateNext) -or
                      ($releaseGateNext -cmatch '^[0-9A-F]{128}$' -and
                       $releaseGateNext -cne $releaseGateCurrent)) `
        'Evidence next release-gate public key is malformed or duplicates current.'
    [string[]]$releaseGateAllowlist = @($evidence.releaseGatePublicKeyAllowlistXY)
    $expectedReleaseGateAllowlist = if ([string]::IsNullOrEmpty($releaseGateNext)) {
        @($releaseGateCurrent)
    } else {
        @($releaseGateCurrent, $releaseGateNext)
    }
    Assert-Condition ($releaseGateAllowlist.Count -eq $expectedReleaseGateAllowlist.Count) `
        'Evidence release-gate public-key allowlist has the wrong size.'
    for ($index = 0; $index -lt $expectedReleaseGateAllowlist.Count; ++$index) {
        Assert-Condition ($releaseGateAllowlist[$index] -ceq $expectedReleaseGateAllowlist[$index]) `
            'Evidence release-gate public-key allowlist is not canonical.'
    }
    if ($IsCandidate) {
        Assert-Condition ($releaseGateCurrent -ceq $ExpectedCurrentReleaseGatePublicKeyXY -and
                          $releaseGateNext -ceq $ExpectedNextReleaseGatePublicKeyXY) `
            'Candidate evidence does not match the configured release-gate public keys.'
    }
    [object[]]$payloadFiles = @($evidence.payloadFiles)
    [void](Get-ExpectedPayloadContract $payloadFiles)
    return [pscustomobject]@{
        Raw = $evidence
        ProductCode = ([guid]([string]$evidence.productCode)).ToString('D').ToUpperInvariant()
        Signer = $signer
        NextSigner = $updaterNext
        SignerAllowlist = $allowlist
        ReleaseGatePublicKeyXY = $releaseGateCurrent
        ReleaseGateNextPublicKeyXY = $releaseGateNext
        ReleaseGatePublicKeyAllowlistXY = $releaseGateAllowlist
        MsiSha256 = [string]$evidence.msiSha256
        PayloadFiles = $payloadFiles
    }
}

function Get-ComProperty {
    param([object] $Object, [string] $Name, [object[]] $Arguments = @())
    return $Object.GetType().InvokeMember(
        $Name, [System.Reflection.BindingFlags]::GetProperty, $null, $Object, $Arguments)
}

function Invoke-ComMethod {
    param([object] $Object, [string] $Name, [object[]] $Arguments = @())
    return $Object.GetType().InvokeMember(
        $Name, [System.Reflection.BindingFlags]::InvokeMethod, $null, $Object, $Arguments)
}

function Get-MsiPropertyMap {
    param([string] $MsiPath)

    $installer = $null
    $database = $null
    $view = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = Invoke-ComMethod $installer 'OpenDatabase' @($MsiPath, 0)
        $view = Invoke-ComMethod $database 'OpenView' @('SELECT `Property`, `Value` FROM `Property`')
        [void](Invoke-ComMethod $view 'Execute')
        $properties = [System.Collections.Generic.Dictionary[string, string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
        while ($true) {
            $record = Invoke-ComMethod $view 'Fetch'
            if ($null -eq $record) { break }
            try {
                $key = [string](Get-ComProperty $record 'StringData' @(1))
                $value = [string](Get-ComProperty $record 'StringData' @(2))
                Assert-Condition (-not [string]::IsNullOrEmpty($key) -and
                                  $properties.TryAdd($key, $value)) `
                    'MSI contains a malformed or duplicate Property row.'
            } finally {
                [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
            }
        }
        return ,$properties
    } finally {
        if ($null -ne $view) {
            try { [void](Invoke-ComMethod $view 'Close') } catch { }
            [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
        }
        if ($null -ne $database) {
            [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
        }
        if ($null -ne $installer) {
            [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
        }
    }
}

function Assert-MsiIdentity {
    param([string] $MsiPath, [object] $Evidence, [string] $Version)

    $properties = Get-MsiPropertyMap $MsiPath
    foreach ($required in @('ProductName', 'Manufacturer', 'ProductVersion', 'ProductCode', 'UpgradeCode')) {
        Assert-Condition ($properties.ContainsKey($required)) "MSI Property table is missing $required."
    }
    $displayName = if ($Architecture -ceq 'x64') {
        "$Product VST3 - Windows x64"
    } else {
        "$Product VST3 - Windows on Arm (ARM64EC)"
    }
    Assert-Condition ($properties['ProductName'] -ceq $displayName) 'MSI ProductName mismatch.'
    Assert-Condition ($properties['Manufacturer'] -ceq $ExpectedManufacturer) `
        'MSI Manufacturer mismatch.'
    Assert-Condition ($properties['ProductVersion'] -ceq $Version) 'MSI ProductVersion mismatch.'
    Assert-Condition ((Normalize-Guid $properties['UpgradeCode'] 'MSI UpgradeCode') -ceq
                      $normalizedUpgradeCode) 'MSI UpgradeCode mismatch.'
    Assert-Condition ((Normalize-Guid $properties['ProductCode'] 'MSI ProductCode') -ceq
                      $Evidence.ProductCode) 'MSI ProductCode differs from signed evidence.'
}

function Assert-AuthenticodeSigner {
    param([string] $Path, [string] $ExpectedPin, [string] $Description)

    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    Assert-Condition ($signature.Status -eq [System.Management.Automation.SignatureStatus]::Valid -and
                      $null -ne $signature.SignerCertificate) `
        "$Description Authenticode signature is invalid: $($signature.Status)"
    Assert-Condition ($null -ne $signature.TimeStamperCertificate) `
        "$Description has no inspectable Authenticode timestamp."
    $actualPin = $signature.SignerCertificate.GetCertHashString(
        [System.Security.Cryptography.HashAlgorithmName]::SHA256).ToUpperInvariant()
    Assert-Condition ($actualPin -ceq $ExpectedPin) "$Description signer pin mismatch."
}

function Get-MsiProductState {
    param([string] $ProductCode)

    $installer = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        return [int](Get-ComProperty $installer 'ProductState' @("{$ProductCode}"))
    } finally {
        if ($null -ne $installer) {
            [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
        }
    }
}

function Invoke-NativeProcess {
    param(
        [string] $Executable,
        [string[]] $Arguments,
        [int] $TimeoutSeconds,
        [string] $Description
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new($Executable)
    $startInfo.UseShellExecute = $false
    foreach ($argument in $Arguments) { [void]$startInfo.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        Assert-Condition ($process.Start()) "$Description could not be started."
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            try { [void]$process.WaitForExit(10000) } catch { }
            throw "$Description timed out after $TimeoutSeconds seconds."
        }
        return $process.ExitCode
    } finally {
        $process.Dispose()
    }
}

function Get-InstallerLogTail {
    param([string] $LogPath)
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        return '<Windows Installer did not create a log>'
    }
    return (Get-Content -LiteralPath $LogPath -Tail 80 | Out-String).Trim()
}

function Invoke-MsiExec {
    param(
        [ValidateSet('/i', '/x')]
        [string] $InstallMode,
        [string] $Target,
        [string] $LogPath,
        [string] $Description
    )
    return Invoke-NativeProcess $msiExec `
        @($InstallMode, $Target, '/qn', '/norestart', '/L*v', $LogPath) `
        $InstallerTimeoutSeconds $Description
}

function Invoke-HostLoad {
    param([string] $Bundle, [string] $Description)
    $exitCode = Invoke-NativeProcess $resolvedHostTest @($Bundle) `
        $HostTestTimeoutSeconds $Description
    Assert-Condition ($exitCode -eq 0) "$Description failed with exit code $exitCode."
}

function New-CryptographicHex {
    param([ValidateRange(1, 1024)][int] $ByteCount)
    [byte[]]$bytes = [byte[]]::new($ByteCount)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return [Convert]::ToHexString($bytes)
}

function Import-ReleaseGateSigningKey {
    param(
        [string] $PrivateKeyPkcs8Base64,
        [string] $ExpectedPublicKeyXY
    )

    Assert-Condition (-not [string]::IsNullOrWhiteSpace($PrivateKeyPkcs8Base64) -and
                      $PrivateKeyPkcs8Base64.Length -le 8192) `
        'Release-gate PKCS#8 secret is missing or exceeds its fixed bound.'
    [byte[]] $privateBytes = $null
    $key = $null
    try {
        $privateBytes = [Convert]::FromBase64String($PrivateKeyPkcs8Base64)
        Assert-Condition ($privateBytes.Length -gt 0 -and $privateBytes.Length -le 4096 -and
                          [Convert]::ToBase64String($privateBytes) -ceq $PrivateKeyPkcs8Base64) `
            'Release-gate PKCS#8 secret is not canonical bounded base64.'
        $key = [System.Security.Cryptography.ECDsa]::Create()
        [int] $bytesRead = 0
        $key.ImportPkcs8PrivateKey($privateBytes, [ref]$bytesRead)
        Assert-Condition ($bytesRead -eq $privateBytes.Length) `
            'Release-gate PKCS#8 secret contains trailing data.'
        $parameters = $key.ExportParameters($false)
        Assert-Condition ($parameters.Q.X.Length -eq 32 -and $parameters.Q.Y.Length -eq 32) `
            'Release-gate private key is not ECDSA P-256.'
        $actualPublicKeyXY = [Convert]::ToHexString($parameters.Q.X) +
            [Convert]::ToHexString($parameters.Q.Y)
        Assert-Condition ($actualPublicKeyXY -ceq $ExpectedPublicKeyXY) `
            'Release-gate private key does not match the configured active public key.'
        return [pscustomobject]@{
            Key = $key
            PublicKeyXY = $actualPublicKeyXY
        }
    } catch {
        if ($null -ne $key) { $key.Dispose() }
        throw
    } finally {
        if ($null -ne $privateBytes) {
            [Array]::Clear($privateBytes, 0, $privateBytes.Length)
        }
    }
}

function New-LowSReleaseGateSignature {
    param(
        [System.Security.Cryptography.ECDsa] $SigningKey,
        [byte[]] $Message
    )

    [byte[]] $signature = $SigningKey.SignData(
        $Message,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
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

function New-ReleaseGateAuthorization {
    param(
        [System.Security.Cryptography.ECDsa] $SigningKey,
        [string] $InstalledVersion,
        [string] $Challenge,
        [string] $PipeName
    )

    $parent = [System.Diagnostics.Process]::GetCurrentProcess()
    try {
        [uint64] $parentCreatedAtFiletime = $parent.StartTime.ToUniversalTime().ToFileTimeUtc()
    } finally {
        $parent.Dispose()
    }
    [uint64] $expiresAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 240
    $message = 'domain=whykiki.windows-updater-release-gate-authorization' + "`n" +
        'schemaVersion=1' + "`n" +
        "repository=$($script:Owner)/$Product" + "`n" +
        "product=$Product" + "`n" +
        "architecture=$Architecture" + "`n" +
        "installedVersion=$InstalledVersion" + "`n" +
        "releaseId=$CandidateReleaseId" + "`n" +
        "tag=$CandidateTag" + "`n" +
        "sourceCommit=$CandidateSourceCommit" + "`n" +
        "challenge=$Challenge" + "`n" +
        "responsePipe=$PipeName" + "`n" +
        "parentProcessId=$PID" + "`n" +
        "parentProcessCreatedAtFiletime=$parentCreatedAtFiletime" + "`n" +
        "expiresAtUnixSeconds=$expiresAt" + "`n"
    [byte[]] $messageBytes = [System.Text.UTF8Encoding]::new($false, $true).GetBytes($message)
    try {
        $signature = New-LowSReleaseGateSignature $SigningKey $messageBytes
    } finally {
        [Array]::Clear($messageBytes, 0, $messageBytes.Length)
    }
    return [pscustomobject]@{
        ParentProcessCreatedAtFiletime = $parentCreatedAtFiletime
        ExpiresAtUnixSeconds = $expiresAt
        SignatureP1363 = $signature
    }
}

function Assert-NamedPipeReleaseGateClient {
    param(
        [System.IO.Pipes.NamedPipeServerStream] $Pipe,
        [System.Diagnostics.Process] $InitialProcess,
        [string] $ExpectedExecutableSha256,
        [string] $ExpectedSigner
    )

    if (-not ('WhykikiAudio.WindowsTransitionNativeMethods' -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace WhykikiAudio
{
    public static class WindowsTransitionNativeMethods
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetNamedPipeClientProcessId(
            SafePipeHandle pipe, out uint clientProcessId);
    }
}
'@
    }
    [uint32]$clientProcessId = 0
    $identified = [WhykikiAudio.WindowsTransitionNativeMethods]::GetNamedPipeClientProcessId(
        $Pipe.SafePipeHandle, [ref]$clientProcessId)
    $nativeError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Assert-Condition $identified `
        "Cannot identify updater release-gate pipe client (Win32 error $nativeError)."
    Assert-Condition ($clientProcessId -ne [uint32]$InitialProcess.Id -and $clientProcessId -gt 0) `
        'Release-gate receipt must come from the copied resume child, not the initial helper.'
    $processRecord = Get-CimInstance -ClassName Win32_Process `
        -Filter "ProcessId = $clientProcessId"
    Assert-Condition ($null -ne $processRecord -and
                      [uint32]$processRecord.ParentProcessId -eq [uint32]$InitialProcess.Id) `
        'Release-gate pipe client is not the direct copied child of the installed updater.'
    $clientProcess = [System.Diagnostics.Process]::GetProcessById([int]$clientProcessId)
    try {
        $clientExecutable = Resolve-SafeFile ($clientProcess.MainModule.FileName) `
            'Copied release-gate updater'
        $operationsFull = [System.IO.Path]::GetFullPath($operationsRoot).TrimEnd('\')
        $clientDirectory = [System.IO.Path]::GetDirectoryName($clientExecutable)
        $clientParent = [System.IO.Path]::GetDirectoryName($clientDirectory)
        $clientDirectoryItem = Get-Item -LiteralPath $clientDirectory -Force
        Assert-Condition ($clientParent -ieq $operationsFull -and
                          [System.IO.Path]::GetFileName($clientExecutable) -ceq "${Product}Updater.exe" -and
                          [System.IO.Path]::GetFileName($clientDirectory) -cmatch
                              '^[0-9A-F]{8}-[0-9A-F]{4}-4[0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}$' -and
                          ($clientDirectoryItem.Attributes -band
                              [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
            'Release-gate pipe client is outside the fresh updater operation.'
        $clientHash = (Get-FileHash -LiteralPath $clientExecutable -Algorithm SHA256).Hash
        Assert-Condition ($clientHash -ceq $ExpectedExecutableSha256) `
            'Copied release-gate pipe client differs from the installed baseline updater.'
        Assert-AuthenticodeSigner $clientExecutable $ExpectedSigner 'Copied release-gate updater'
    } finally {
        $clientProcess.Dispose()
    }
}

function Invoke-ReleaseGateUpdater {
    param(
        [string] $UpdaterPath,
        [string] $InstalledVersion,
        [string] $TargetVersion,
        [string] $ExpectedMsiSha256,
        [string] $Challenge,
        [string] $PipeName,
        [uint64] $ParentProcessCreatedAtFiletime,
        [uint64] $ExpiresAtUnixSeconds,
        [string] $AuthorizationSignatureP1363,
        [string] $ExpectedUpdaterSha256,
        [string] $ExpectedUpdaterSigner
    )

    Assert-Condition ($Challenge -cmatch '^[0-9A-F]{64}$') `
        'Release-gate challenge is not canonical.'
    Assert-Condition ($PipeName -cmatch '^WhykikiAudio\.UpdaterReleaseGate\.[0-9A-F]{32}$') `
        'Release-gate pipe name is not canonical.'
    Assert-Condition ($ParentProcessCreatedAtFiletime -gt 0) `
        'Release-gate parent process creation time is not canonical.'
    [uint64] $nowUnixSeconds = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    Assert-Condition ($ExpiresAtUnixSeconds -gt $nowUnixSeconds -and
                      $ExpiresAtUnixSeconds -le $nowUnixSeconds + 300) `
        'Release-gate authorization expiry is outside the permitted lifetime.'
    Assert-Condition ($AuthorizationSignatureP1363 -cmatch '^[0-9A-F]{128}$') `
        'Release-gate authorization signature is not canonical P-256 P1363.'
    $pipeOptions = [System.IO.Pipes.PipeOptions](
        [int][System.IO.Pipes.PipeOptions]::Asynchronous -bor
        [int][System.IO.Pipes.PipeOptions]::CurrentUserOnly)
    $pipe = [System.IO.Pipes.NamedPipeServerStream]::new(
        $PipeName,
        [System.IO.Pipes.PipeDirection]::In,
        1,
        [System.IO.Pipes.PipeTransmissionMode]::Byte,
        $pipeOptions,
        4096,
        4096)
    $process = $null
    try {
        $connection = $pipe.WaitForConnectionAsync()
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new($UpdaterPath)
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        [void]$startInfo.Environment.Remove('WINDOWS_RELEASE_GATE_PRIVATE_KEY_PKCS8_BASE64')
        $arguments = @(
            '--release-gate',
            '--challenge', $Challenge,
            '--response-pipe', $PipeName,
            '--parent-process-id', ([string]$PID),
            '--release-id', ([string]$CandidateReleaseId),
            '--tag', $CandidateTag,
            '--source-commit', $CandidateSourceCommit,
            '--parent-process-created-at-filetime', ([string]$ParentProcessCreatedAtFiletime),
            '--expires-at-unix-seconds', ([string]$ExpiresAtUnixSeconds),
            '--authorization-signature-p1363', $AuthorizationSignatureP1363
        )
        foreach ($argument in $arguments) { [void]$startInfo.ArgumentList.Add($argument) }
        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        Assert-Condition ($process.Start()) 'Installed updater could not be started.'

        $deadline = [DateTime]::UtcNow.AddSeconds($UpdaterTimeoutSeconds)
        while (-not $connection.IsCompleted -and -not $process.HasExited -and
               [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 100
        }
        Assert-Condition ($connection.IsCompleted) `
            'Installed updater exited or timed out before connecting to the response pipe.'
        $connection.GetAwaiter().GetResult()
        Assert-NamedPipeReleaseGateClient $pipe $process $ExpectedUpdaterSha256 `
            $ExpectedUpdaterSigner
        $expected = [ordered]@{
            schema = $script:ReleaseGateSchema
            schemaVersion = 1
            challenge = $Challenge
            serverProcessId = $PID
            product = $Product
            installedVersion = $InstalledVersion
            targetVersion = $TargetVersion
            architecture = $Architecture
            releaseId = $CandidateReleaseId
            sourceCommit = $CandidateSourceCommit
            msiSha256 = $ExpectedMsiSha256
            phase = 'verified'
        }
        $expectedText = ($expected | ConvertTo-Json -Compress) + "`n"
        [byte[]]$expectedBytes = [System.Text.UTF8Encoding]::new($false).GetBytes($expectedText)
        Assert-Condition ($expectedBytes.Length -le 4096) `
            'Expected updater receipt exceeds the transport bound.'
        [byte[]]$received = [byte[]]::new($expectedBytes.Length + 1)
        $receivedCount = 0
        while ($receivedCount -lt $received.Length) {
            $remaining = [int][Math]::Max(
                0, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMilliseconds))
            Assert-Condition ($remaining -gt 0) 'Installed updater release gate timed out.'
            $readTask = $pipe.ReadAsync($received, $receivedCount, $received.Length - $receivedCount)
            Assert-Condition ($readTask.Wait($remaining)) 'Updater response pipe read timed out.'
            $read = [int]$readTask.GetAwaiter().GetResult()
            if ($read -eq 0) { break }
            $receivedCount += $read
        }
        $remaining = [int][Math]::Max(
            0, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalMilliseconds))
        Assert-Condition ($remaining -gt 0 -and $process.WaitForExit($remaining)) `
            'Installed updater release gate did not exit in time.'
        Assert-Condition ($process.ExitCode -eq 0) `
            "Installed updater failed with exit code $($process.ExitCode)."
        Assert-Condition ($receivedCount -eq $expectedBytes.Length) `
            "Updater returned a non-canonical receipt length: $receivedCount."
        $byteDifference = 0
        for ($index = 0; $index -lt $expectedBytes.Length; ++$index) {
            $byteDifference = $byteDifference -bor ($received[$index] -bxor $expectedBytes[$index])
        }
        Assert-Condition ($byteDifference -eq 0) `
            'Updater response is not the exact canonical verified receipt.'
        return ($expectedText | ConvertFrom-Json)
    } finally {
        if ($null -ne $process) {
            if (-not $process.HasExited) {
                try { $process.Kill($true) } catch { }
                try { [void]$process.WaitForExit(10000) } catch { }
            }
            $process.Dispose()
        }
        $pipe.Dispose()
    }
}

function Remove-VerifiedOperationDirectories {
    param(
        [string] $OperationsRoot,
        [string] $Challenge,
        [string] $PipeName,
        [string] $AuthorizationSignatureP1363
    )

    if (-not (Test-Path -LiteralPath $OperationsRoot)) { return 0 }
    $root = Get-Item -LiteralPath $OperationsRoot -Force
    Assert-Condition ($root -is [System.IO.DirectoryInfo] -and
                      ($root.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
        'Updater Operations root is missing, linked, or not a directory.'
    $operations = @(Get-ChildItem -LiteralPath $root.FullName -Directory -Force)
    Assert-Condition ($operations.Count -le 1) `
        'Updater created an ambiguous number of operation directories.'
    foreach ($operation in $operations) {
        Assert-Condition ($operation.Name -cmatch '^[0-9A-F]{8}-[0-9A-F]{4}-4[0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}$') `
            'Updater operation directory name is not a canonical version-4 GUID.'
        $entries = @(Get-ChildItem -LiteralPath $operation.FullName -Recurse -Force)
        foreach ($entry in $entries) {
            Assert-Condition (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
                'Updater operation cleanup encountered a reparse point.'
        }
        $journalPath = Join-Path $operation.FullName 'journal.json'
        Assert-Condition (Test-Path -LiteralPath $journalPath -PathType Leaf) `
            'Updater verified operation has no journal.'
        $journalText = Get-Content -LiteralPath $journalPath -Raw -Encoding utf8
        $journal = $journalText | ConvertFrom-Json
        Assert-Condition ([string]$journal.operationMode -ceq 'release-gate' -and
                          [string]$journal.phase -ceq 'verified' -and
                          [string]$journal.releaseId -ceq ([string]$CandidateReleaseId) -and
                          [string]$journal.releaseTag -ceq $CandidateTag -and
                          [string]$journal.sourceCommit -ceq $CandidateSourceCommit) `
            'Updater operation journal did not reach verified.'
        Assert-Condition ($journalText -cnotmatch [regex]::Escape($Challenge) -and
                          $journalText -cnotmatch [regex]::Escape($PipeName) -and
                          $journalText -cnotmatch [regex]::Escape($AuthorizationSignatureP1363) -and
                          $journalText -cnotmatch 'authorizationSignature|expiresAtUnixSeconds|parentProcessCreatedAtFiletime' -and
                          $journalText -cnotmatch '(?i)private.?key|pkcs8|WINDOWS_RELEASE_GATE_PRIVATE_KEY') `
            'Ephemeral release-gate authorization leaked into the updater journal.'
        Assert-Condition (@(Get-ChildItem -LiteralPath $operation.FullName -Recurse -File -Force |
            Where-Object { $_.Extension -ieq '.msi' }).Count -eq 0) `
            'Verified updater operation retained a downloaded MSI.'
        $privateTemp = Join-Path $operation.FullName 'private-temp'
        if (Test-Path -LiteralPath $privateTemp) {
            Assert-Condition (@(Get-ChildItem -LiteralPath $privateTemp -Force).Count -eq 0) `
                'Verified updater operation retained private temporary payload.'
        }
        Remove-Item -LiteralPath $operation.FullName -Recurse -Force
        Assert-Condition (-not (Test-Path -LiteralPath $operation.FullName)) `
            'Test-owned updater operation cleanup failed.'
    }
    return $operations.Count
}

if (-not $IsWindows) { throw 'Windows updater transition acceptance must run on Windows.' }
Assert-Condition ($Product -ceq 'SubLab808') 'This transition gate is bound to SubLab808.'
Assert-Condition ($ExpectedManufacturer -ceq 'Whykiki Audio') `
    'This transition gate is bound to the production manufacturer.'
Assert-Condition ($CandidateTag -cmatch `
    '^v(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$') `
    'CandidateTag is not canonical.'
$candidateVersion = $CandidateTag.Substring(1)
$candidateTuple = ConvertTo-VersionTuple $candidateVersion 'Candidate version'
Assert-Condition ($CandidateSourceCommit -cmatch '^[0-9a-f]{40}$') `
    'CandidateSourceCommit must be lowercase hexadecimal.'
$normalizedUpgradeCode = Normalize-Guid $ExpectedUpgradeCode 'ExpectedUpgradeCode'
$normalizedOtherUpgradeCode = Normalize-Guid `
    $ExpectedOtherArchitectureUpgradeCode 'ExpectedOtherArchitectureUpgradeCode'
Assert-Condition ($ExpectedUpgradeCode -ceq $normalizedUpgradeCode -and
                  $ExpectedOtherArchitectureUpgradeCode -ceq $normalizedOtherUpgradeCode -and
                  $normalizedUpgradeCode -cne $normalizedOtherUpgradeCode) `
    'Independent UpgradeCodes must be canonical, uppercase, and distinct.'
$architectureMsiContract = if ($Architecture -ceq 'x64') { 'x64' } else { 'arm64' }
Assert-Condition ($ExpectedMsiArchitecture -ceq $architectureMsiContract) `
    'ExpectedMsiArchitecture is inconsistent with Architecture.'
$expectedPin = $ExpectedSignerSha256.Replace(' ', '').ToUpperInvariant()
$expectedNextPin = ([string]$ExpectedNextSignerSha256).Replace(' ', '').ToUpperInvariant()
Assert-Condition ($expectedPin -cmatch '^[0-9A-F]{64}$') `
    'ExpectedSignerSha256 must be canonical.'
Assert-Condition ([string]::IsNullOrEmpty($expectedNextPin) -or
                  ($expectedNextPin -cmatch '^[0-9A-F]{64}$' -and
                   $expectedNextPin -cne $expectedPin)) `
    'ExpectedNextSignerSha256 must be empty or a distinct canonical pin.'
$expectedReleaseGatePublicKeyXY = [string]$ExpectedReleaseGatePublicKeyXY
$expectedReleaseGateNextPublicKeyXY = [string]$ExpectedReleaseGateNextPublicKeyXY
Assert-Condition ($expectedReleaseGatePublicKeyXY -cmatch '^[0-9A-F]{128}$') `
    'ExpectedReleaseGatePublicKeyXY must be canonical P-256 X||Y.'
Assert-Condition ([string]::IsNullOrEmpty($expectedReleaseGateNextPublicKeyXY) -or
                  ($expectedReleaseGateNextPublicKeyXY -cmatch '^[0-9A-F]{128}$' -and
                   $expectedReleaseGateNextPublicKeyXY -cne $expectedReleaseGatePublicKeyXY)) `
    'ExpectedReleaseGateNextPublicKeyXY must be empty or a distinct canonical P-256 X||Y.'
$releaseGatePrivateKeyText = [string]$ReleaseGatePrivateKeyPkcs8Base64
$ReleaseGatePrivateKeyPkcs8Base64 = ''
Remove-Item Env:WINDOWS_RELEASE_GATE_PRIVATE_KEY_PKCS8_BASE64 -ErrorAction SilentlyContinue
Assert-Condition (-not [string]::IsNullOrWhiteSpace($releaseGatePrivateKeyText)) `
    'ReleaseGatePrivateKeyPkcs8Base64 is required.'

if ($Mode -ceq 'bootstrap') {
    Assert-Condition ($BaselineReleaseId -eq 0 -and
                      [string]::IsNullOrEmpty($BaselineTag) -and
                      [string]::IsNullOrEmpty($BaselineVersion) -and
                      [string]::IsNullOrEmpty($BaselineSourceCommit)) `
        'Bootstrap mode must not carry a baseline identity.'
} else {
    Assert-Condition ($BaselineReleaseId -gt 0) 'Upgrade mode requires a baseline release ID.'
    Assert-Condition ($BaselineTag -ceq "v$BaselineVersion") `
        'Baseline tag and version differ.'
    $baselineTuple = ConvertTo-VersionTuple $BaselineVersion 'Baseline version'
    Assert-Condition ((Compare-VersionTuple $candidateTuple $baselineTuple) -gt 0) `
        'Candidate is not strictly newer than the baseline.'
    Assert-Condition ($BaselineSourceCommit -cmatch '^[0-9a-f]{40}$') `
        'BaselineSourceCommit must be lowercase hexadecimal.'
}

$identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [System.Security.Principal.WindowsPrincipal]::new($identity)
Assert-Condition ($principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) `
    'Transition acceptance requires an elevated Windows runner.'
$resolvedHostTest = Resolve-SafeFile $HostTestPath 'Native host test'
$resolvedCleanInstallGate = Resolve-SafeFile $CleanInstallGatePath 'Trusted clean-install gate'
$receiptFull = [System.IO.Path]::GetFullPath($ReceiptPath)
Assert-Condition (-not (Test-Path -LiteralPath $receiptFull)) 'Transition receipt already exists.'
$receiptParent = [System.IO.Path]::GetDirectoryName($receiptFull)
Assert-Condition (-not [string]::IsNullOrWhiteSpace($receiptParent) -and
                  (Test-Path -LiteralPath $receiptParent -PathType Container)) `
    'Transition receipt parent directory is unavailable.'

$commonFiles = [Environment]::GetEnvironmentVariable('CommonProgramFiles', 'Process')
Assert-Condition (-not [string]::IsNullOrWhiteSpace($commonFiles)) 'CommonProgramFiles is unavailable.'
$vst3Root = [System.IO.Path]::GetFullPath((Join-Path $commonFiles 'VST3'))
$installedBundle = [System.IO.Path]::GetFullPath((Join-Path $vst3Root "$Product.vst3"))
Assert-Condition ($installedBundle.StartsWith(
        $vst3Root.TrimEnd([char[]]@('\', '/')) + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) `
    'Derived VST3 path escaped Common Files.'
$msiExec = Resolve-SafeFile (Join-Path $env:SystemRoot 'System32\msiexec.exe') `
    'Windows Installer client'
$runnerTemp = [Environment]::GetEnvironmentVariable('RUNNER_TEMP', 'Process')
Assert-Condition (-not [string]::IsNullOrWhiteSpace($runnerTemp)) 'RUNNER_TEMP is required.'
$workRoot = Join-Path ([System.IO.Path]::GetFullPath($runnerTemp)) `
    "$Product-transition-$([guid]::NewGuid().ToString('N'))"
$localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
Assert-Condition (-not [string]::IsNullOrWhiteSpace($localAppData)) 'LocalAppData is unavailable.'
$operationsRoot = Join-Path $localAppData "Whykiki Audio\$Product\Updater\Operations"

$primaryError = $null
$cleanupErrors = [System.Collections.Generic.List[string]]::new()
$candidateEvidence = $null
$baselineEvidence = $null
$candidateMsiLease = $null
$baselineMsiLease = $null
$cleanupAuthorized = $false
$operationChallenge = ''
$operationPipeName = ''
$operationAuthorizationSignature = ''

try {
    [System.IO.Directory]::CreateDirectory($workRoot) | Out-Null
    Assert-Condition (-not (Test-Path -LiteralPath $installedBundle)) `
        'Transition runner is not clean; target VST3 already exists.'
    if (Test-Path -LiteralPath $operationsRoot) {
        $operationsRootItem = Get-Item -LiteralPath $operationsRoot -Force
        Assert-Condition (($operationsRootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0 -and
                          @(Get-ChildItem -LiteralPath $operationsRoot -Force).Count -eq 0) `
            'Transition runner has pre-existing updater operations.'
    }

    $candidateRelease = Get-PublicRelease $CandidateReleaseId
    $candidateAssets = Assert-ReleaseContract $candidateRelease $CandidateReleaseId `
        $CandidateTag $CandidateSourceCommit $true $candidateVersion
    $candidateMsiName = "$Product-$candidateVersion-Windows-$Architecture.msi"
    $candidateEvidenceName = "$Product-$candidateVersion-Windows-$Architecture.evidence.json"
    $candidateMsi = Receive-VerifiedAsset $candidateAssets[$candidateMsiName] `
        (Join-Path $workRoot $candidateMsiName) 256MB
    $candidateEvidencePath = Receive-VerifiedAsset $candidateAssets[$candidateEvidenceName] `
        (Join-Path $workRoot $candidateEvidenceName) 8MB
    $candidateMsiLease = [System.IO.File]::Open(
        $candidateMsi, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    $candidateEvidence = Read-EvidenceContract $candidateEvidencePath $candidateVersion `
        $CandidateSourceCommit $candidateMsiName $expectedPin $expectedNextPin `
        $expectedReleaseGatePublicKeyXY $expectedReleaseGateNextPublicKeyXY $true
    Assert-Condition ((Get-FileHash -LiteralPath $candidateMsi -Algorithm SHA256).Hash -ceq
                      $candidateEvidence.MsiSha256) 'Candidate MSI differs from evidence.'
    Assert-AuthenticodeSigner $candidateMsi $candidateEvidence.Signer 'Candidate MSI'
    Assert-MsiIdentity $candidateMsi $candidateEvidence $candidateVersion
    Assert-Condition ((Get-MsiProductState $candidateEvidence.ProductCode) -eq $script:InstallStateUnknown) `
        'Candidate ProductCode is already registered.'
    $cleanupAuthorized = $true

    if ($Mode -ceq 'bootstrap') {
        $bootstrapSigningKeyContract = Import-ReleaseGateSigningKey $releaseGatePrivateKeyText `
            $expectedReleaseGatePublicKeyXY
        $bootstrapSigningKeyContract.Key.Dispose()
        $releaseGatePrivateKeyText = ''
        $cleanArguments = @{
            MsiPath = $candidateMsi
            EvidencePath = $candidateEvidencePath
            HostTestPath = $resolvedHostTest
            Product = $Product
            Architecture = $Architecture
            ExpectedMsiArchitecture = $ExpectedMsiArchitecture
            ExpectedVersion = $candidateVersion
            ExpectedManufacturer = $ExpectedManufacturer
            ExpectedUpgradeCode = $normalizedUpgradeCode
            ExpectedOtherArchitectureUpgradeCode = $normalizedOtherUpgradeCode
            ExpectedSignerSha256 = $expectedPin
            ExpectedReleaseGatePublicKeyXY = $expectedReleaseGatePublicKeyXY
            InstallerTimeoutSeconds = $InstallerTimeoutSeconds
            HostTestTimeoutSeconds = $HostTestTimeoutSeconds
        }
        if (-not [string]::IsNullOrEmpty($expectedNextPin)) {
            $cleanArguments['ExpectedNextSignerSha256'] = $expectedNextPin
        }
        if (-not [string]::IsNullOrEmpty($expectedReleaseGateNextPublicKeyXY)) {
            $cleanArguments['ExpectedReleaseGateNextPublicKeyXY'] =
                $expectedReleaseGateNextPublicKeyXY
        }
        & $resolvedCleanInstallGate @cleanArguments
        Assert-Condition ($?) 'Trusted clean-install gate failed.'
        Assert-Condition ((Get-MsiProductState $candidateEvidence.ProductCode) -eq
                          $script:InstallStateUnknown -and
                          -not (Test-Path -LiteralPath $installedBundle)) `
            'Bootstrap clean-install gate did not restore a clean machine.'
        $transitionReceipt = [ordered]@{
            schema = $script:TransitionSchema
            schemaVersion = 1
            mode = $Mode
            product = $Product
            architecture = $Architecture
            baseline = $null
            candidate = [ordered]@{
                releaseId = $CandidateReleaseId
                tag = $CandidateTag
                sourceCommit = $CandidateSourceCommit
                version = $candidateVersion
                msiSha256 = $candidateEvidence.MsiSha256
            }
            checks = [ordered]@{
                cleanInstall = $true
                exactPayload = $true
                hostLoad = $true
                productCleanup = $true
            }
        }
    } else {
        $baselineRelease = Get-PublicRelease $BaselineReleaseId
        $baselineAssets = Assert-ReleaseContract $baselineRelease $BaselineReleaseId `
            $BaselineTag $BaselineSourceCommit $false $BaselineVersion
        $baselineMsiName = "$Product-$BaselineVersion-Windows-$Architecture.msi"
        $baselineEvidenceName = "$Product-$BaselineVersion-Windows-$Architecture.evidence.json"
        $baselineMsi = Receive-VerifiedAsset $baselineAssets[$baselineMsiName] `
            (Join-Path $workRoot $baselineMsiName) 256MB
        $baselineEvidencePath = Receive-VerifiedAsset $baselineAssets[$baselineEvidenceName] `
            (Join-Path $workRoot $baselineEvidenceName) 8MB
        $baselineMsiLease = [System.IO.File]::Open(
            $baselineMsi, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read)
        $baselineEvidence = Read-EvidenceContract $baselineEvidencePath $BaselineVersion `
            $BaselineSourceCommit $baselineMsiName '' '' '' '' $false
        Assert-Condition ($baselineEvidence.SignerAllowlist -ccontains $candidateEvidence.Signer) `
            'Installed baseline updater evidence does not allow the candidate signer.'
        Assert-Condition ($baselineEvidence.ReleaseGatePublicKeyAllowlistXY -ccontains
                          $expectedReleaseGatePublicKeyXY) `
            'Installed baseline updater evidence does not allow the active release-gate key.'
        Assert-Condition ((Get-FileHash -LiteralPath $baselineMsi -Algorithm SHA256).Hash -ceq
                          $baselineEvidence.MsiSha256) 'Baseline MSI differs from evidence.'
        Assert-AuthenticodeSigner $baselineMsi $baselineEvidence.Signer 'Baseline MSI'
        Assert-MsiIdentity $baselineMsi $baselineEvidence $BaselineVersion
        Assert-Condition ($baselineEvidence.ProductCode -cne $candidateEvidence.ProductCode) `
            'Baseline and candidate ProductCodes must differ.'
        Assert-Condition ((Get-MsiProductState $baselineEvidence.ProductCode) -eq
                          $script:InstallStateUnknown) 'Baseline ProductCode is already registered.'

        $baselineInstallLog = Join-Path $workRoot 'baseline-install.log'
        $baselineInstallExit = Invoke-MsiExec '/i' $baselineMsi $baselineInstallLog `
            'Exact public baseline installation'
        if ($baselineInstallExit -ne 0) {
            throw "Baseline installation failed with exit code $baselineInstallExit.`n$(Get-InstallerLogTail $baselineInstallLog)"
        }
        Assert-Condition ((Get-MsiProductState $baselineEvidence.ProductCode) -eq
                          $script:InstallStateDefault) `
            'Baseline ProductState is not INSTALLSTATE_DEFAULT.'
        Assert-InstalledPayloadMatchesEvidence $installedBundle $baselineEvidence.PayloadFiles
        Invoke-HostLoad $installedBundle 'Installed public baseline host load'

        $updaterRelative = "Contents\Helpers\${Product}Updater.exe"
        Assert-Condition ($baselineEvidence.Raw.updaterPaths.Count -eq 1 -and
                          [string]$baselineEvidence.Raw.updaterPaths[0] -ceq $updaterRelative) `
            'Baseline evidence does not identify exactly the production updater.'
        Assert-Condition ($baselineEvidence.PayloadFiles.path -ccontains $updaterRelative) `
            'Baseline payload evidence does not contain the updater.'
        $installedUpdater = Resolve-SafeFile (Join-Path $installedBundle $updaterRelative) `
            'Installed baseline updater'
        $updaterEvidence = @($baselineEvidence.PayloadFiles |
            Where-Object { [string]$_.path -ceq $updaterRelative })
        Assert-Condition ($updaterEvidence.Count -eq 1 -and
                          (Get-FileHash -LiteralPath $installedUpdater -Algorithm SHA256).Hash -ceq
                              [string]$updaterEvidence[0].sha256) `
            'Installed updater bytes differ from baseline evidence.'
        Assert-AuthenticodeSigner $installedUpdater $baselineEvidence.Signer `
            'Installed baseline updater'

        $challenge = New-CryptographicHex 32
        $pipeName = "WhykikiAudio.UpdaterReleaseGate.$(New-CryptographicHex 16)"
        $operationChallenge = $challenge
        $operationPipeName = $pipeName
        $signingKeyContract = Import-ReleaseGateSigningKey $releaseGatePrivateKeyText `
            $expectedReleaseGatePublicKeyXY
        try {
            $authorization = New-ReleaseGateAuthorization $signingKeyContract.Key `
                $BaselineVersion $challenge $pipeName
        } finally {
            $signingKeyContract.Key.Dispose()
            $releaseGatePrivateKeyText = ''
        }
        $operationAuthorizationSignature = $authorization.SignatureP1363
        $installedUpdaterSha256 = (Get-FileHash -LiteralPath $installedUpdater -Algorithm SHA256).Hash
        $gateReceipt = Invoke-ReleaseGateUpdater $installedUpdater $BaselineVersion `
            $candidateVersion $candidateEvidence.MsiSha256 $challenge $pipeName `
            $authorization.ParentProcessCreatedAtFiletime $authorization.ExpiresAtUnixSeconds `
            $authorization.SignatureP1363 `
            $installedUpdaterSha256 $baselineEvidence.Signer

        Assert-Condition ((Get-MsiProductState $baselineEvidence.ProductCode) -eq
                          $script:InstallStateUnknown) `
            'Baseline ProductCode remains installed after the updater transition.'
        Assert-Condition ((Get-MsiProductState $candidateEvidence.ProductCode) -eq
                          $script:InstallStateDefault) `
            'Candidate ProductCode is not INSTALLSTATE_DEFAULT after the updater transition.'
        Assert-InstalledPayloadMatchesEvidence $installedBundle $candidateEvidence.PayloadFiles
        Invoke-HostLoad $installedBundle 'Updater-installed candidate host load'

        $downgradeLog = Join-Path $workRoot 'downgrade-rejection.log'
        $downgradeExit = Invoke-MsiExec '/i' $baselineMsi $downgradeLog `
            'Exact baseline downgrade attempt'
        Assert-Condition ($downgradeExit -notin @(0, 3010)) `
            'Older baseline MSI was not rejected as a downgrade.'
        Assert-Condition ((Get-MsiProductState $baselineEvidence.ProductCode) -eq
                          $script:InstallStateUnknown -and
                          (Get-MsiProductState $candidateEvidence.ProductCode) -eq
                              $script:InstallStateDefault) `
            'Downgrade attempt changed registered product state.'
        Assert-InstalledPayloadMatchesEvidence $installedBundle $candidateEvidence.PayloadFiles
        Invoke-HostLoad $installedBundle 'Post-downgrade-rejection candidate host load'

        $operationCount = Remove-VerifiedOperationDirectories $operationsRoot `
            $operationChallenge $operationPipeName $operationAuthorizationSignature
        Assert-Condition ($operationCount -eq 1) `
            'Updater did not leave exactly one verified test-owned operation to audit and clean.'

        $transitionReceipt = [ordered]@{
            schema = $script:TransitionSchema
            schemaVersion = 1
            mode = $Mode
            product = $Product
            architecture = $Architecture
            baseline = [ordered]@{
                releaseId = $BaselineReleaseId
                tag = $BaselineTag
                sourceCommit = $BaselineSourceCommit
                version = $BaselineVersion
                msiSha256 = $baselineEvidence.MsiSha256
            }
            candidate = [ordered]@{
                releaseId = $CandidateReleaseId
                tag = $CandidateTag
                sourceCommit = $CandidateSourceCommit
                version = $candidateVersion
                msiSha256 = $candidateEvidence.MsiSha256
            }
            updaterReceipt = $gateReceipt
            checks = [ordered]@{
                baselineInstall = $true
                installedBaselineUpdater = $true
                publicPrereleaseLookup = $true
                exactVerifiedReceipt = $true
                exactCandidatePayload = $true
                candidateHostLoad = $true
                downgradeRejected = $true
                updaterOperationCleanup = $true
                productCleanup = $true
            }
        }
    }
}
catch {
    $primaryError = $_
}
finally {
    if ($cleanupAuthorized -and $null -ne $candidateEvidence) {
        foreach ($contract in @($candidateEvidence, $baselineEvidence)) {
            if ($null -eq $contract) { continue }
            try {
                $state = Get-MsiProductState $contract.ProductCode
                if ($state -ne $script:InstallStateUnknown) {
                    $cleanupLog = Join-Path $workRoot "cleanup-$($contract.ProductCode).log"
                    $cleanupExit = Invoke-MsiExec '/x' "{$($contract.ProductCode)}" $cleanupLog `
                        "Product-specific cleanup $($contract.ProductCode)"
                    if ($cleanupExit -notin @(0, 1605, 3010)) {
                        throw "Cleanup returned $cleanupExit.`n$(Get-InstallerLogTail $cleanupLog)"
                    }
                }
                Assert-Condition ((Get-MsiProductState $contract.ProductCode) -eq
                                  $script:InstallStateUnknown) `
                    "ProductCode remains registered after cleanup: $($contract.ProductCode)"
            } catch {
                $cleanupErrors.Add($_.Exception.Message)
            }
        }
        try {
            Assert-Condition (-not (Test-Path -LiteralPath $installedBundle)) `
                'VST3 bundle remains after product-specific cleanup.'
        } catch {
            $cleanupErrors.Add($_.Exception.Message)
        }
    }
    if ($null -ne $candidateMsiLease) {
        try { $candidateMsiLease.Dispose() } catch { $cleanupErrors.Add($_.Exception.Message) }
    }
    if ($null -ne $baselineMsiLease) {
        try { $baselineMsiLease.Dispose() } catch { $cleanupErrors.Add($_.Exception.Message) }
    }
    if (Test-Path -LiteralPath $workRoot -PathType Container) {
        try { Remove-Item -LiteralPath $workRoot -Recurse -Force }
        catch { $cleanupErrors.Add("Transition work directory cleanup failed: $($_.Exception.Message)") }
    }
}

$failures = [System.Collections.Generic.List[string]]::new()
if ($null -ne $primaryError) {
    $failures.Add("Windows transition acceptance failed: $($primaryError.Exception.Message)")
}
foreach ($cleanupError in $cleanupErrors) { $failures.Add("Cleanup failed: $cleanupError") }
if ($failures.Count -gt 0) { throw ($failures -join "`n") }

$receiptJson = $transitionReceipt | ConvertTo-Json -Depth 8 -Compress
[System.IO.File]::WriteAllText(
    $receiptFull, $receiptJson + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Windows $Mode acceptance passed for $Product ${Architecture}; receipt: $receiptFull"
