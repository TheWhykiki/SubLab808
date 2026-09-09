#requires -Version 7.2

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $MsiPath,

    [Parameter(Mandatory = $true)]
    [string] $EvidencePath,

    [Parameter(Mandatory = $true)]
    [string] $HostTestPath,

    [Parameter(Mandatory = $true)]
    [string] $Product,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64ec')]
    [string] $Architecture,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64')]
    [string] $ExpectedMsiArchitecture,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedVersion,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedManufacturer,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedUpgradeCode,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedOtherArchitectureUpgradeCode,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedSignerSha256,

    [string] $ExpectedNextSignerSha256,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedReleaseGatePublicKeyXY,

    [string] $ExpectedReleaseGateNextPublicKeyXY,

    [ValidateRange(1, 3600)]
    [int] $InstallerTimeoutSeconds = 300,

    [ValidateRange(1, 3600)]
    [int] $HostTestTimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $IsWindows) {
    throw 'The installed-MSI acceptance gate must run on Windows.'
}

$script:InstallStateUnknown = -1
$script:InstallStateDefault = 5

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

function Assert-NoAlternateDataStreams {
    param([string] $Path, [string] $DisplayPath)

    $streams = @(Get-Item -LiteralPath $Path -Stream '*' -ErrorAction Stop)
    $unexpected = @($streams | Where-Object { $_.Stream -notin @(':$DATA', '$DATA') })
    Assert-Condition ($unexpected.Count -eq 0) `
        "Alternate data streams are forbidden in the installed payload: $DisplayPath"
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
                continue
            }

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

    return [pscustomobject]@{
        Root = $rootFull
        Directories = $directories.ToArray()
        Files = $files
    }
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
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($relative)) `
            'Payload evidence contains an empty path.'
        Assert-Condition ($relative -cnotmatch '/' -and $relative -cnotmatch ':' -and
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
        try { $length = [long]$entry.size }
        catch { throw "Payload evidence contains an invalid size: $relative" }
        Assert-Condition ($length -ge 0 -and ([string]$length -ceq ([string]$entry.size))) `
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

function Assert-InstalledPayloadMatchesEvidence {
    param([string] $InstalledBundle, [object[]] $PayloadFiles)

    $expected = Get-ExpectedPayloadContract $PayloadFiles
    $actual = Get-SafeTreeSnapshot $InstalledBundle
    Assert-Condition ($actual.Directories.Count -eq $expected.Directories.Count) `
        'Installed payload directory count differs from the signed evidence.'
    foreach ($directory in $actual.Directories) {
        Assert-Condition ($expected.Directories.Contains($directory)) `
            "Unexpected installed payload directory: $directory"
    }
    Assert-Condition ($actual.Files.Count -eq $expected.Files.Count) `
        'Installed payload file count differs from the signed evidence.'
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

function Normalize-Guid {
    param([string] $Value, [string] $Description)

    Assert-Condition ($Value -cmatch `
        '^\{?[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[1-5][0-9A-Fa-f]{3}-[89ABab][0-9A-Fa-f]{3}-[0-9A-Fa-f]{12}\}?$') `
        "$Description is not a valid GUID."
    return ([guid]$Value).ToString('D').ToUpperInvariant()
}

function Get-MsiRows {
    param([object] $Database, [string] $Query)

    $view = Invoke-ComMethod $Database 'OpenView' @($Query)
    $rows = [System.Collections.Generic.List[object]]::new()
    try {
        [void](Invoke-ComMethod $view 'Execute')
        while ($true) {
            $record = Invoke-ComMethod $view 'Fetch'
            if ($null -eq $record) { break }
            try {
                $fieldCount = [int](Get-ComProperty $record 'FieldCount')
                [string[]]$fields = for ($index = 1; $index -le $fieldCount; ++$index) {
                    [string](Get-ComProperty $record 'StringData' @($index))
                }
                $rows.Add([pscustomobject]@{ Fields = $fields })
            }
            finally {
                [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
            }
        }
    }
    finally {
        try { [void](Invoke-ComMethod $view 'Close') } catch { }
        [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    }
    return $rows.ToArray()
}

function Get-MsiSummaryProperty {
    param([object] $Installer, [string] $MsiPath, [int] $PropertyId)

    $summary = Get-ComProperty $Installer 'SummaryInformation' @($MsiPath, 0)
    try { return [string](Get-ComProperty $summary 'Property' @($PropertyId)) }
    finally { [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($summary) }
}

function Assert-MsiUpgradeRow {
    param(
        [object] $Row,
        [string] $ExpectedRowUpgradeCode,
        [string] $ExpectedVersionMin,
        [string] $ExpectedVersionMax,
        [string] $ExpectedLanguage,
        [int] $ExpectedAttributes,
        [string] $ExpectedActionProperty
    )

    Assert-Condition ($Row.Fields.Count -eq 7) "Malformed Upgrade row: $ExpectedActionProperty"
    Assert-Condition ((Normalize-Guid $Row.Fields[0] "UpgradeCode for $ExpectedActionProperty") -ceq
                      $ExpectedRowUpgradeCode) `
        "UpgradeCode mismatch for $ExpectedActionProperty."
    Assert-Condition ($Row.Fields[1] -ceq $ExpectedVersionMin -and
                      $Row.Fields[2] -ceq $ExpectedVersionMax -and
                      $Row.Fields[3] -ceq $ExpectedLanguage -and
                      [int]($Row.Fields[4]) -eq $ExpectedAttributes -and
                      $Row.Fields[5] -ceq '' -and
                      $Row.Fields[6] -ceq $ExpectedActionProperty) `
        "Upgrade row semantics mismatch for $ExpectedActionProperty."
}

function Get-MsiIdentityContract {
    param(
        [string] $MsiPath,
        [string] $Product,
        [string] $ArtifactArchitecture,
        [string] $ExpectedVersion,
        [string] $ExpectedManufacturer,
        [string] $ExpectedUpgradeCode,
        [string] $ExpectedOtherArchitectureUpgradeCode,
        [string] $ExpectedMsiArchitecture
    )

    $installer = $null
    $database = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = Invoke-ComMethod $installer 'OpenDatabase' @($MsiPath, 0)
        $properties = [System.Collections.Generic.Dictionary[string, string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
        foreach ($row in @(Get-MsiRows $database 'SELECT `Property`, `Value` FROM `Property`')) {
            Assert-Condition ($row.Fields.Count -eq 2 -and $row.Fields[0] -and
                              -not $properties.ContainsKey($row.Fields[0])) `
                'Malformed or duplicate MSI Property row.'
            $properties.Add($row.Fields[0], $row.Fields[1])
        }
        foreach ($requiredProperty in @('ProductName', 'Manufacturer', 'ProductVersion',
                                         'ProductCode', 'UpgradeCode')) {
            Assert-Condition ($properties.ContainsKey($requiredProperty)) `
                "MSI Property table is missing $requiredProperty."
        }

        $expectedDisplayName = if ($ArtifactArchitecture -ceq 'x64') {
            "$Product VST3 - Windows x64"
        } else {
            "$Product VST3 - Windows on Arm (ARM64EC)"
        }
        Assert-Condition ($properties['ProductName'] -ceq $expectedDisplayName) `
            'MSI ProductName does not match the independently expected product and architecture.'
        Assert-Condition ($properties['Manufacturer'] -ceq $ExpectedManufacturer) `
            'MSI Manufacturer does not match the independently expected manufacturer.'
        Assert-Condition ($properties['ProductVersion'] -ceq $ExpectedVersion) `
            'MSI ProductVersion does not match the independently expected release version.'
        Assert-Condition ((Normalize-Guid $properties['UpgradeCode'] 'MSI UpgradeCode') -ceq
                          $ExpectedUpgradeCode) `
            'MSI UpgradeCode does not match the independently expected architecture identity.'
        $databaseProductCode = Normalize-Guid $properties['ProductCode'] 'MSI ProductCode'

        $template = Get-MsiSummaryProperty $installer $MsiPath 7
        $templatePlatform = $template.Split(';')[0]
        if ($ExpectedMsiArchitecture -ceq 'arm64') {
            Assert-Condition ($templatePlatform -ieq 'Arm64') `
                "Expected an Arm64 MSI Summary template, found '$template'."
        } else {
            Assert-Condition ($templatePlatform -in @('x64', 'Intel64')) `
                "Expected an x64 MSI Summary template, found '$template'."
        }

        $upgradeRows = @(Get-MsiRows $database `
            'SELECT `UpgradeCode`, `VersionMin`, `VersionMax`, `Language`, `Attributes`, `Remove`, `ActionProperty` FROM `Upgrade`')
        Assert-Condition ($upgradeRows.Count -eq 3) `
            "MSI has $($upgradeRows.Count) Upgrade rows; expected exactly 3."
        $upgradeByProperty = [System.Collections.Generic.Dictionary[string, object]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($row in $upgradeRows) {
            Assert-Condition ($row.Fields.Count -eq 7 -and $row.Fields[6] -and
                              $upgradeByProperty.TryAdd($row.Fields[6], $row)) `
                'Malformed or duplicate MSI Upgrade row.'
        }
        foreach ($requiredActionProperty in @('WIX_UPGRADE_DETECTED', 'WIX_DOWNGRADE_DETECTED',
                                               'OTHERARCHITECTUREDETECTED')) {
            Assert-Condition ($upgradeByProperty.ContainsKey($requiredActionProperty)) `
                "MSI Upgrade row is missing: $requiredActionProperty"
        }
        Assert-MsiUpgradeRow $upgradeByProperty['WIX_UPGRADE_DETECTED'] $ExpectedUpgradeCode '' `
            $ExpectedVersion '1033' 1 'WIX_UPGRADE_DETECTED'
        Assert-MsiUpgradeRow $upgradeByProperty['WIX_DOWNGRADE_DETECTED'] $ExpectedUpgradeCode `
            $ExpectedVersion '' '1033' 2 'WIX_DOWNGRADE_DETECTED'
        Assert-MsiUpgradeRow $upgradeByProperty['OTHERARCHITECTUREDETECTED'] `
            $ExpectedOtherArchitectureUpgradeCode '0.0.0' '' '' 258 'OTHERARCHITECTUREDETECTED'

        return [pscustomobject]@{
            ProductCode = $databaseProductCode
            ProductName = $properties['ProductName']
            Manufacturer = $properties['Manufacturer']
            ProductVersion = $properties['ProductVersion']
            UpgradeCode = $ExpectedUpgradeCode
            OtherArchitectureUpgradeCode = $ExpectedOtherArchitectureUpgradeCode
            MsiArchitecture = $ExpectedMsiArchitecture
        }
    }
    finally {
        if ($null -ne $database) {
            [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
        }
        if ($null -ne $installer) {
            [void][System.Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
        }
    }
}

function Get-MsiProductState {
    param([string] $ProductCode)

    $installer = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        return [int](Get-ComProperty $installer 'ProductState' @($ProductCode))
    }
    finally {
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
    }
    finally {
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
        [string] $MsiExec,
        [ValidateSet('/i', '/x')]
        [string] $Mode,
        [string] $Target,
        [string] $LogPath,
        [int] $TimeoutSeconds,
        [string] $Description
    )

    return Invoke-NativeProcess $MsiExec `
        @($Mode, $Target, '/qn', '/norestart', '/L*v', $LogPath) `
        $TimeoutSeconds $Description
}

Assert-Condition ($Product -cmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$') `
    'Product must be a safe canonical identifier.'
Assert-Condition ($ExpectedVersion -cmatch `
    '^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$') `
    'ExpectedVersion must be a canonical MSI version.'
Assert-Condition ($ExpectedManufacturer -cmatch '^[A-Za-z0-9][A-Za-z0-9._ -]{0,99}$') `
    'ExpectedManufacturer must be a safe non-empty manufacturer identity.'
$normalizedExpectedUpgradeCode = Normalize-Guid $ExpectedUpgradeCode 'ExpectedUpgradeCode'
$normalizedExpectedOtherUpgradeCode = Normalize-Guid `
    $ExpectedOtherArchitectureUpgradeCode 'ExpectedOtherArchitectureUpgradeCode'
Assert-Condition ($ExpectedUpgradeCode -ceq $normalizedExpectedUpgradeCode) `
    'ExpectedUpgradeCode must be a canonical uppercase GUID.'
Assert-Condition ($ExpectedOtherArchitectureUpgradeCode -ceq $normalizedExpectedOtherUpgradeCode) `
    'ExpectedOtherArchitectureUpgradeCode must be a canonical uppercase GUID.'
Assert-Condition ($normalizedExpectedUpgradeCode -cne $normalizedExpectedOtherUpgradeCode) `
    'Current and other-architecture UpgradeCodes must be distinct.'
$architectureMsiContract = if ($Architecture -ceq 'x64') { 'x64' } else { 'arm64' }
Assert-Condition ($ExpectedMsiArchitecture -ceq $architectureMsiContract) `
    'ExpectedMsiArchitecture is inconsistent with the artifact architecture.'
$expectedPin = $ExpectedSignerSha256.Replace(' ', '').ToUpperInvariant()
Assert-Condition ($expectedPin -cmatch '^[0-9A-F]{64}$') `
    'ExpectedSignerSha256 must be exactly 64 hexadecimal characters.'
$expectedNextPin = ([string]$ExpectedNextSignerSha256).Replace(' ', '').ToUpperInvariant()
Assert-Condition ([string]::IsNullOrEmpty($expectedNextPin) -or
                  $expectedNextPin -cmatch '^[0-9A-F]{64}$') `
    'ExpectedNextSignerSha256 must be empty or exactly 64 hexadecimal characters.'
Assert-Condition ([string]::IsNullOrEmpty($expectedNextPin) -or $expectedNextPin -cne $expectedPin) `
    'ExpectedNextSignerSha256 must differ from ExpectedSignerSha256.'
$expectedReleaseGatePublicKey = [string]$ExpectedReleaseGatePublicKeyXY
Assert-Condition ($expectedReleaseGatePublicKey -cmatch '^[0-9A-F]{128}\z') `
    'ExpectedReleaseGatePublicKeyXY must be exactly 128 uppercase hexadecimal characters.'
$expectedReleaseGateNextPublicKey = [string]$ExpectedReleaseGateNextPublicKeyXY
Assert-Condition ([string]::IsNullOrEmpty($expectedReleaseGateNextPublicKey) -or
                  $expectedReleaseGateNextPublicKey -cmatch '^[0-9A-F]{128}\z') `
    'ExpectedReleaseGateNextPublicKeyXY must be empty or exactly 128 uppercase hexadecimal characters.'
Assert-Condition ([string]::IsNullOrEmpty($expectedReleaseGateNextPublicKey) -or
                  $expectedReleaseGateNextPublicKey -cne $expectedReleaseGatePublicKey) `
    'ExpectedReleaseGateNextPublicKeyXY must differ from ExpectedReleaseGatePublicKeyXY.'

$identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [System.Security.Principal.WindowsPrincipal]::new($identity)
Assert-Condition ($principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) `
    'The installed-MSI acceptance gate requires an elevated Windows runner.'

$resolvedMsi = Resolve-SafeFile $MsiPath 'MSI candidate'
$resolvedEvidence = Resolve-SafeFile $EvidencePath 'MSI evidence'
$resolvedHostTest = Resolve-SafeFile $HostTestPath 'Native VST3 host test'
$commonFiles = [Environment]::GetEnvironmentVariable('CommonProgramFiles', 'Process')
Assert-Condition (-not [string]::IsNullOrWhiteSpace($commonFiles)) `
    'CommonProgramFiles is unavailable on this Windows runner.'
$vst3Root = [System.IO.Path]::GetFullPath((Join-Path $commonFiles 'VST3'))
$installedBundle = [System.IO.Path]::GetFullPath((Join-Path $vst3Root "$Product.vst3"))
Assert-Condition ($installedBundle.StartsWith(
        $vst3Root.TrimEnd([char[]]@('\', '/')) + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) `
    'Derived VST3 installation path escaped Common Files.'

$msiExec = Join-Path $env:SystemRoot 'System32\msiexec.exe'
$msiExec = Resolve-SafeFile $msiExec 'Windows Installer client'
$runnerTemp = [Environment]::GetEnvironmentVariable('RUNNER_TEMP', 'Process')
Assert-Condition (-not [string]::IsNullOrWhiteSpace($runnerTemp)) `
    'RUNNER_TEMP is required for isolated Windows Installer logs.'
$workRoot = Join-Path ([System.IO.Path]::GetFullPath($runnerTemp)) `
    "$Product-msi-acceptance-$([guid]::NewGuid().ToString('N'))"

$cleanupAuthorized = $false
$installAttempted = $false
$primaryError = $null
$cleanupErrors = [System.Collections.Generic.List[string]]::new()
$productCode = $null
$msiLease = $null

try {
    [System.IO.Directory]::CreateDirectory($workRoot) | Out-Null
    # Deny write and delete sharing from before byte/identity verification until every
    # msiexec operation has ended, so the elevated install cannot reopen different bytes.
    $msiLease = [System.IO.File]::Open(
        $resolvedMsi,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    $evidence = Get-Content -LiteralPath $resolvedEvidence -Raw -Encoding utf8 | ConvertFrom-Json
    Assert-Condition ($evidence.schemaVersion -eq 4) 'Unsupported Windows installer evidence schema.'
    Assert-Condition ([string]$evidence.artifactStatus -ceq 'SIGNED' -and
                      $evidence.signed -is [bool] -and $evidence.signed) `
        'Installed-MSI acceptance requires a signed production candidate.'
    Assert-Condition ([string]$evidence.product -ceq $Product) 'Evidence product mismatch.'
    Assert-Condition ([string]$evidence.payloadArchitecture -ceq $Architecture) `
        'Evidence architecture mismatch.'
    Assert-Condition ([string]$evidence.msiArchitecture -ceq $ExpectedMsiArchitecture) `
        'Evidence MSI architecture does not match the independent workflow input.'
    Assert-Condition ([string]$evidence.upgradeCode -ceq $normalizedExpectedUpgradeCode -and
                      [string]$evidence.otherArchitectureUpgradeCode -ceq
                          $normalizedExpectedOtherUpgradeCode) `
        'Evidence UpgradeCodes do not match the independent workflow inputs.'
    Assert-Condition ([string]$evidence.validation.manufacturer -ceq $ExpectedManufacturer) `
        'Evidence manufacturer does not match the independent workflow input.'
    Assert-Condition ([string]$evidence.msiFile -ceq [System.IO.Path]::GetFileName($resolvedMsi)) `
        'Evidence MSI filename mismatch.'
    Assert-Condition ([string]$evidence.version -ceq $ExpectedVersion) `
        'Evidence version does not match the independent workflow input.'
    $expectedMsiName = "$Product-$ExpectedVersion-Windows-$Architecture.msi"
    Assert-Condition ([System.IO.Path]::GetFileName($resolvedMsi) -ceq $expectedMsiName) `
        'MSI filename is not canonical for the evidence identity.'
    Assert-Condition ([string]$evidence.productCode -cmatch `
        '^[0-9A-F]{8}-[0-9A-F]{4}-[1-5][0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}$') `
        'Evidence ProductCode is not a canonical uppercase GUID.'
    $productCode = ([guid]$evidence.productCode).ToString('B').ToUpperInvariant()

    $evidenceSigner = ([string]$evidence.signerCertificateSha256).Replace(' ', '').ToUpperInvariant()
    $updaterPin = ([string]$evidence.updaterCurrentSignerSha256).Replace(' ', '').ToUpperInvariant()
    $updaterNextPin = ([string]$evidence.updaterNextSignerSha256).Replace(' ', '').ToUpperInvariant()
    $expectedAllowlist = if ([string]::IsNullOrEmpty($expectedNextPin)) {
        @($expectedPin)
    } else {
        @($expectedPin, $expectedNextPin)
    }
    [string[]]$evidenceAllowlist = @($evidence.payloadSignerAllowlistSha256)
    $allowlistMatches = $evidenceAllowlist.Count -eq $expectedAllowlist.Count
    if ($allowlistMatches) {
        for ($index = 0; $index -lt $expectedAllowlist.Count; ++$index) {
            if ($evidenceAllowlist[$index] -cne $expectedAllowlist[$index]) {
                $allowlistMatches = $false
                break
            }
        }
    }
    Assert-Condition ($evidenceSigner -ceq $expectedPin -and $updaterPin -ceq $expectedPin) `
        'Evidence signer identities do not match the configured release pin.'
    Assert-Condition ($updaterNextPin -ceq $expectedNextPin -and $allowlistMatches) `
        'Evidence payload signer allowlist does not match the configured current/next pins.'
    $evidencePropertyNames = @($evidence.PSObject.Properties.Name)
    foreach ($releaseGateEvidenceField in @(
        'releaseGatePublicKeyXY',
        'releaseGateNextPublicKeyXY',
        'releaseGatePublicKeyAllowlistXY'
    )) {
        Assert-Condition ($evidencePropertyNames -ccontains $releaseGateEvidenceField) `
            "Evidence is missing required release-gate field '$releaseGateEvidenceField'."
    }
    Assert-Condition ($evidence.releaseGatePublicKeyXY -is [string] -and
                      [string]$evidence.releaseGatePublicKeyXY -ceq
                          $expectedReleaseGatePublicKey) `
        'Evidence release-gate public key does not match the configured current key.'
    if ([string]::IsNullOrEmpty($expectedReleaseGateNextPublicKey)) {
        Assert-Condition ($null -eq $evidence.releaseGateNextPublicKeyXY) `
            'Evidence next release-gate public key must be null when no next key is configured.'
        [string[]]$expectedReleaseGateAllowlist = @($expectedReleaseGatePublicKey)
    } else {
        Assert-Condition ($evidence.releaseGateNextPublicKeyXY -is [string] -and
                          [string]$evidence.releaseGateNextPublicKeyXY -ceq
                              $expectedReleaseGateNextPublicKey) `
            'Evidence next release-gate public key does not match the configured next key.'
        [string[]]$expectedReleaseGateAllowlist = @(
            $expectedReleaseGatePublicKey,
            $expectedReleaseGateNextPublicKey
        )
    }
    $releaseGateAllowlistValue = $evidence.releaseGatePublicKeyAllowlistXY
    $releaseGateAllowlistMatches = $releaseGateAllowlistValue -is [System.Array] -and
        $releaseGateAllowlistValue.Count -eq $expectedReleaseGateAllowlist.Count
    if ($releaseGateAllowlistMatches) {
        for ($index = 0; $index -lt $expectedReleaseGateAllowlist.Count; ++$index) {
            if (-not ($releaseGateAllowlistValue[$index] -is [string]) -or
                $releaseGateAllowlistValue[$index] -cne $expectedReleaseGateAllowlist[$index]) {
                $releaseGateAllowlistMatches = $false
                break
            }
        }
    }
    Assert-Condition $releaseGateAllowlistMatches `
        'Evidence release-gate public-key allowlist does not exactly match the configured current/next keys.'
    $actualMsiHash = (Get-FileHash -LiteralPath $resolvedMsi -Algorithm SHA256).Hash.ToUpperInvariant()
    Assert-Condition ([string]$evidence.msiSha256 -ceq $actualMsiHash) `
        'MSI bytes do not match the signed evidence.'

    $signature = Get-AuthenticodeSignature -LiteralPath $resolvedMsi
    Assert-Condition ($signature.Status -eq [System.Management.Automation.SignatureStatus]::Valid -and
                      $null -ne $signature.SignerCertificate) `
        "MSI Authenticode signature is invalid: $($signature.Status)"
    Assert-Condition ($null -ne $signature.TimeStamperCertificate) `
        'MSI has no inspectable Authenticode timestamp certificate.'
    $actualSigner = $signature.SignerCertificate.GetCertHashString(
        [System.Security.Cryptography.HashAlgorithmName]::SHA256).ToUpperInvariant()
    Assert-Condition ($actualSigner -ceq $expectedPin) `
        'MSI Authenticode signer does not match the configured SHA-256 leaf pin.'
    $msiIdentity = Get-MsiIdentityContract $resolvedMsi $Product $Architecture `
        $ExpectedVersion $ExpectedManufacturer $normalizedExpectedUpgradeCode `
        $normalizedExpectedOtherUpgradeCode $ExpectedMsiArchitecture
    $databaseProductCode = $msiIdentity.ProductCode
    Assert-Condition ($databaseProductCode -ceq ([string]$evidence.productCode)) `
        'Signed MSI ProductCode does not match the release evidence.'
    $productCode = ([guid]$databaseProductCode).ToString('B').ToUpperInvariant()

    [object[]]$payloadFiles = @($evidence.payloadFiles)
    [void](Get-ExpectedPayloadContract $payloadFiles)
    Assert-Condition ((Get-MsiProductState $productCode) -eq $script:InstallStateUnknown) `
        "Candidate ProductCode is already registered: $productCode"
    Assert-Condition (-not (Test-Path -LiteralPath $installedBundle)) `
        "The acceptance runner is not clean; target bundle already exists: $installedBundle"
    $cleanupAuthorized = $true

    $installAttempted = $true
    $installLog = Join-Path $workRoot 'install.log'
    $installExit = Invoke-MsiExec $msiExec '/i' $resolvedMsi $installLog `
        $InstallerTimeoutSeconds 'Silent MSI installation'
    # The MSI contract forbids reboot actions, so 3010 is not a release success.
    if ($installExit -ne 0) {
        throw "Silent MSI installation failed with exit code $installExit.`n$(Get-InstallerLogTail $installLog)"
    }
    Assert-Condition ((Get-MsiProductState $productCode) -eq $script:InstallStateDefault) `
        'MSI ProductState is not INSTALLSTATE_DEFAULT after installation.'
    Assert-InstalledPayloadMatchesEvidence $installedBundle $payloadFiles

    $hostExit = Invoke-NativeProcess $resolvedHostTest @($installedBundle) `
        $HostTestTimeoutSeconds 'Installed VST3 host test'
    Assert-Condition ($hostExit -eq 0) `
        "Native host test rejected the installed VST3 bundle with exit code $hostExit."

    $uninstallLog = Join-Path $workRoot 'uninstall.log'
    $uninstallExit = Invoke-MsiExec $msiExec '/x' $productCode $uninstallLog `
        $InstallerTimeoutSeconds 'Silent MSI uninstallation'
    # Cleanup may tolerate 3010 only to continue verifying final state; acceptance may not.
    if ($uninstallExit -ne 0) {
        throw "Silent MSI uninstallation failed with exit code $uninstallExit.`n$(Get-InstallerLogTail $uninstallLog)"
    }
    Assert-Condition ((Get-MsiProductState $productCode) -eq $script:InstallStateUnknown) `
        'MSI ProductState remains registered after uninstallation.'
    Assert-Condition (-not (Test-Path -LiteralPath $installedBundle)) `
        "Installed VST3 bundle remains after uninstallation: $installedBundle"
}
catch {
    $primaryError = $_
}
finally {
    if ($cleanupAuthorized -and $installAttempted) {
        $cleanupDiagnostics = [System.Collections.Generic.List[string]]::new()
        $cleanupVerified = $false
        for ($attempt = 1; $attempt -le 3; ++$attempt) {
            try {
                $state = Get-MsiProductState $productCode
                $bundleExists = Test-Path -LiteralPath $installedBundle
                if ($state -eq $script:InstallStateUnknown -and -not $bundleExists) {
                    $cleanupVerified = $true
                    break
                }
            }
            catch {
                $cleanupDiagnostics.Add("Cleanup state query $attempt failed: $($_.Exception.Message)")
            }

            $cleanupLog = Join-Path $workRoot "cleanup-$attempt.log"
            try {
                $cleanupExit = Invoke-MsiExec $msiExec '/x' $productCode $cleanupLog `
                    $InstallerTimeoutSeconds "MSI cleanup attempt $attempt"
                if ($cleanupExit -notin @(0, 1605, 3010)) {
                    $cleanupDiagnostics.Add(
                        "Cleanup attempt $attempt returned $cleanupExit.`n$(Get-InstallerLogTail $cleanupLog)")
                }
            }
            catch {
                $cleanupDiagnostics.Add("Cleanup attempt $attempt failed: $($_.Exception.Message)")
            }
            if ($attempt -lt 3) { Start-Sleep -Seconds 2 }
        }

        try {
            $finalState = Get-MsiProductState $productCode
            if ($finalState -ne $script:InstallStateUnknown) {
                throw "ProductState is $finalState instead of INSTALLSTATE_UNKNOWN."
            }
            Assert-Condition (-not (Test-Path -LiteralPath $installedBundle)) `
                "Residual VST3 bundle remains after product-specific MSI cleanup: $installedBundle"
            $cleanupVerified = $true
        }
        catch {
            $cleanupErrors.Add("Installed-MSI cleanup could not be verified: $($_.Exception.Message)")
        }
        if (-not $cleanupVerified -and $cleanupDiagnostics.Count -gt 0) {
            $cleanupErrors.Add(($cleanupDiagnostics -join "`n"))
        }
    }

    if ($null -ne $msiLease) {
        try { $msiLease.Dispose() }
        catch { $cleanupErrors.Add("Verified MSI lease cleanup failed: $($_.Exception.Message)") }
    }

    if (-not [string]::IsNullOrWhiteSpace($workRoot) -and
        (Test-Path -LiteralPath $workRoot -PathType Container)) {
        try { Remove-Item -LiteralPath $workRoot -Recurse -Force }
        catch { $cleanupErrors.Add("Acceptance log cleanup failed: $($_.Exception.Message)") }
    }
}

$failures = [System.Collections.Generic.List[string]]::new()
if ($null -ne $primaryError) { $failures.Add("Acceptance failed: $($primaryError.Exception.Message)") }
foreach ($cleanupError in $cleanupErrors) { $failures.Add("Cleanup failed: $cleanupError") }
if ($failures.Count -gt 0) { throw ($failures -join "`n") }

Write-Host "Installed-MSI acceptance passed for $Product ${Architecture}: install, exact payload, host load and uninstall."
