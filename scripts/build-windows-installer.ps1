#requires -Version 7.2

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('x64', 'arm64ec')]
    [string] $Architecture,

    [Parameter(Mandatory)]
    [string] $BundlePath,

    [Parameter(Mandatory)]
    [ValidatePattern('^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$')]
    [string] $Version,

    [string] $SourceCommit,
    [string] $OutputDirectory = (Join-Path $PSScriptRoot '..\dist\windows'),
    [string[]] $UpdaterPath = @(),
    [string] $ExpectedSignerSha256,
    [string] $HostTestPath,
    [string] $DumpbinPath,
    [string] $SignToolPath,
    [string] $CertificateThumbprint,
    [string] $CertificateSubject,
    [string] $CertificateStoreName = 'My',
    [switch] $UseMachineCertificateStore,
    [string] $TimestampUrl,
    [ValidateRange(1, 3600)]
    [int] $AdministrativeExtractionTimeoutSeconds = 300,
    [ValidateRange(1, 3600)]
    [int] $HostTestTimeoutSeconds = 300,
    [switch] $AllowUnsigned
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if (-not $IsWindows) {
    throw 'Windows MSI packages must be built and validated on Windows.'
}

$script:RequiredWixVersion = '6.0.2'
$script:RequiredNuGetSignerFingerprint = 'D95336DD2022934D80E3F3A4F938DD66EC7076BBBA680F76C11F2B54B346D61D'
$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:InstallerRoot = Join-Path $script:RepoRoot 'Installer\Windows'
$script:ToolManifest = Join-Path $script:RepoRoot '.config\dotnet-tools.json'
$script:NuGetConfig = Join-Path $script:InstallerRoot 'NuGet.Config'
$script:PackageSource = Join-Path $script:InstallerRoot 'Package.wxs'
$script:PackageConfig = Join-Path $script:InstallerRoot 'package-config.json'

function Assert-Condition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw $Message }
}

function Get-FullPath {
    param([string] $Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Resolve-ExistingFile {
    param([string] $Path, [string] $Description)
    Assert-Condition ($Path) "$Description path is required."
    $full = Get-FullPath $Path
    Assert-Condition (Test-Path -LiteralPath $full -PathType Leaf) "$Description was not found: $full"
    $item = Get-Item -LiteralPath $full -Force
    Assert-Condition (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Description must not be a reparse point: $full"
    return $full
}

function Get-RelativePathInside {
    param([string] $Root, [string] $Candidate, [string] $Description)
    $rootFull = (Get-FullPath $Root).TrimEnd([char[]]@('\', '/')) + [System.IO.Path]::DirectorySeparatorChar
    $candidateFull = Get-FullPath $Candidate
    Assert-Condition ($candidateFull.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) `
        "$Description must be inside the VST3 bundle: $candidateFull"
    return [System.IO.Path]::GetRelativePath($rootFull, $candidateFull).Replace('/', '\')
}

function Assert-NoAlternateDataStreams {
    param([string] $Path, [string] $DisplayPath)
    $streams = @(Get-Item -LiteralPath $Path -Stream '*' -ErrorAction Stop)
    $unexpectedStreams = @($streams | Where-Object { $_.Stream -notin @(':$DATA', '$DATA') })
    Assert-Condition ($unexpectedStreams.Count -eq 0) `
        "Alternate data streams are forbidden in MSI payloads: $DisplayPath"
}

function Get-SafeTreeSnapshot {
    param([string] $Root)

    $rootFull = Get-FullPath $Root
    Assert-Condition (Test-Path -LiteralPath $rootFull -PathType Container) "Directory was not found: $rootFull"
    $rootItem = Get-Item -LiteralPath $rootFull -Force
    Assert-Condition (($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
        "Reparse-point roots are forbidden: $rootFull"
    Assert-NoAlternateDataStreams $rootFull '<bundle-root>'

    $pending = [System.Collections.Generic.Queue[System.IO.DirectoryInfo]]::new()
    $pending.Enqueue($rootItem)
    $directoryPaths = [System.Collections.Generic.List[string]]::new()
    $pathKeys = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $fileByPath = [System.Collections.Generic.Dictionary[string, object]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)

    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($entry in $directory.EnumerateFileSystemInfos()) {
            $relative = [System.IO.Path]::GetRelativePath($rootFull, $entry.FullName).Replace('/', '\')
            Assert-Condition (-not $relative.StartsWith('..')) "Entry escaped payload root: $($entry.FullName)"
            Assert-Condition (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
                "Reparse points, symlinks and junctions are forbidden in MSI payloads: $relative"
            Assert-Condition ($pathKeys.Add($relative)) "Case-colliding payload path: $relative"
            Assert-NoAlternateDataStreams $entry.FullName $relative

            if ($entry -is [System.IO.DirectoryInfo]) {
                $directoryPaths.Add($relative)
                $pending.Enqueue($entry)
                continue
            }
            Assert-Condition ($entry -is [System.IO.FileInfo]) "Unsupported filesystem entry in payload: $relative"

            $fileByPath.Add($relative, [pscustomobject]@{
                RelativePath = $relative
                FullName = $entry.FullName
                Length = $entry.Length
                Sha256 = (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
            })
        }
    }

    [string[]] $directories = $directoryPaths.ToArray()
    [Array]::Sort($directories, [System.StringComparer]::Ordinal)
    [string[]] $filePaths = @($fileByPath.Keys)
    [Array]::Sort($filePaths, [System.StringComparer]::Ordinal)
    [object[]] $files = @($filePaths | ForEach-Object { $fileByPath[$_] })
    return [pscustomobject]@{ Root = $rootFull; Directories = $directories; Files = $files }
}

function Assert-SameSnapshot {
    param([object] $Expected, [object] $Actual, [string] $Context)
    Assert-Condition ($Expected.Directories.Count -eq $Actual.Directories.Count) `
        "$Context directory count differs."
    for ($index = 0; $index -lt $Expected.Directories.Count; ++$index) {
        Assert-Condition ($Expected.Directories[$index] -ceq $Actual.Directories[$index]) `
            "$Context directory differs: '$($Expected.Directories[$index])' vs '$($Actual.Directories[$index])'"
    }
    Assert-Condition ($Expected.Files.Count -eq $Actual.Files.Count) "$Context file count differs."
    for ($index = 0; $index -lt $Expected.Files.Count; ++$index) {
        $left = $Expected.Files[$index]
        $right = $Actual.Files[$index]
        Assert-Condition ($left.RelativePath -ceq $right.RelativePath) `
            "$Context file path differs: '$($left.RelativePath)' vs '$($right.RelativePath)'"
        Assert-Condition ($left.Length -eq $right.Length -and $left.Sha256 -ceq $right.Sha256) `
            "$Context file bytes differ: $($left.RelativePath)"
    }
}

function Copy-Snapshot {
    param([object] $Snapshot, [string] $Destination)
    [System.IO.Directory]::CreateDirectory($Destination) | Out-Null
    foreach ($relative in $Snapshot.Directories) {
        [System.IO.Directory]::CreateDirectory((Join-Path $Destination $relative)) | Out-Null
    }
    foreach ($file in $Snapshot.Files) {
        $target = Join-Path $Destination $file.RelativePath
        [System.IO.File]::Copy($file.FullName, $target, $false)
    }
}

function Get-PeImageClassification {
    param([string] $Path, [string] $DisplayPath)

    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    try {
        if ($stream.Length -lt 2) {
            return [pscustomobject]@{ HasMzHeader = $false; IsPortableExecutable = $false }
        }
        [byte[]] $dosMagic = [byte[]]::new(2)
        Assert-Condition ($stream.Read($dosMagic, 0, $dosMagic.Length) -eq $dosMagic.Length) `
            "Could not read payload header: $DisplayPath"
        if ($dosMagic[0] -ne 0x4d -or $dosMagic[1] -ne 0x5a) {
            return [pscustomobject]@{ HasMzHeader = $false; IsPortableExecutable = $false }
        }

        Assert-Condition ($stream.Length -ge 64) "Truncated MZ executable payload: $DisplayPath"
        [void]$stream.Seek(0x3c, [System.IO.SeekOrigin]::Begin)
        [byte[]] $offsetBytes = [byte[]]::new(4)
        Assert-Condition ($stream.Read($offsetBytes, 0, $offsetBytes.Length) -eq $offsetBytes.Length) `
            "Could not read PE header offset: $DisplayPath"
        [uint32] $peOffset = [BitConverter]::ToUInt32($offsetBytes, 0)
        Assert-Condition ($peOffset -ge 64 -and [uint64]$peOffset + 4 -le [uint64]$stream.Length) `
            "MZ payload has an invalid PE header offset: $DisplayPath"
        [void]$stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin)
        [byte[]] $peMagic = [byte[]]::new(4)
        Assert-Condition ($stream.Read($peMagic, 0, $peMagic.Length) -eq $peMagic.Length) `
            "Could not read PE signature: $DisplayPath"
        Assert-Condition ($peMagic[0] -eq 0x50 -and $peMagic[1] -eq 0x45 -and
                          $peMagic[2] -eq 0x00 -and $peMagic[3] -eq 0x00) `
            "MZ payload does not contain a valid PE signature: $DisplayPath"
        return [pscustomobject]@{ HasMzHeader = $true; IsPortableExecutable = $true }
    }
    finally {
        $stream.Dispose()
    }
}

function Get-PayloadExecutableContract {
    param(
        [object] $Snapshot,
        [string] $ProductName,
        [string] $ExpectedPluginBinary,
        [bool] $UnsignedMode
    )

    $forbiddenExecutableExtensions = @(
        '.application', '.appref-ms', '.bat', '.bash', '.cmd', '.com', '.cpl', '.fish',
        '.gadget', '.hta', '.inf', '.ins', '.isp', '.jar', '.js', '.jse', '.lnk', '.lua',
        '.msc', '.msi', '.msp', '.mst', '.ocx', '.php', '.pif', '.pl', '.ps1', '.ps1xml',
        '.ps2', '.ps2xml', '.psc1', '.psc2', '.psd1', '.psm1', '.py', '.pyw', '.rb',
        '.reg', '.scf', '.scr', '.sct', '.sh', '.shb', '.sys', '.url', '.vb', '.vbe',
        '.vbs', '.ws', '.wsc', '.wsf', '.wsh', '.zsh'
    )
    $peRequiredExtensions = @('.dll', '.exe', '.vst3')
    $pePaths = [System.Collections.Generic.List[string]]::new()
    foreach ($file in $Snapshot.Files) {
        $extension = [System.IO.Path]::GetExtension($file.RelativePath).ToLowerInvariant()
        Assert-Condition ($forbiddenExecutableExtensions -cnotcontains $extension) `
            "Script, installer or dangerous executable extension is forbidden in a VST3 payload: $($file.RelativePath)"
        $classification = Get-PeImageClassification $file.FullName $file.RelativePath
        if ($peRequiredExtensions -ccontains $extension) {
            Assert-Condition ($classification.HasMzHeader -and $classification.IsPortableExecutable) `
                "Executable payload extension does not contain an MZ/PE image: $($file.RelativePath)"
        }
        if ($classification.IsPortableExecutable) { $pePaths.Add($file.RelativePath) }
    }

    [string[]] $portableExecutables = $pePaths.ToArray()
    [Array]::Sort($portableExecutables, [System.StringComparer]::Ordinal)
    Assert-Condition ($portableExecutables -ccontains $ExpectedPluginBinary) `
        "Expected plug-in binary is not a structurally valid PE image: $ExpectedPluginBinary"

    $expectedUpdater = "Contents\Helpers\$($ProductName)Updater.exe"
    [string[]] $helperExecutables = @($portableExecutables | Where-Object {
        $_.StartsWith('Contents\Helpers\', [System.StringComparison]::OrdinalIgnoreCase)
    })
    [string[]] $updaterLikeExecutables = @($portableExecutables | Where-Object {
        [System.IO.Path]::GetFileNameWithoutExtension($_) -match '(?i)(update|upgrade|installer|setup|bootstrap|patch)'
    })
    if ($UnsignedMode) {
        Assert-Condition ($portableExecutables.Count -eq 1 -and
                          $portableExecutables[0] -ceq $ExpectedPluginBinary -and
                          $helperExecutables.Count -eq 0 -and
                          $updaterLikeExecutables.Count -eq 0) `
            '-AllowUnsigned payloads must contain only the exact primary VST3 PE and no embedded updater/additional PE helper.'
    } else {
        Assert-Condition ($portableExecutables -ccontains $expectedUpdater) `
            "Production payload is missing the exact updater PE: $expectedUpdater"
        Assert-Condition ($updaterLikeExecutables.Count -eq 1 -and
                          $updaterLikeExecutables[0] -ceq $expectedUpdater) `
            'Production payloads require exactly one updater-like PE helper: Contents\Helpers\<Product>Updater.exe.'
    }

    return [pscustomobject]@{
        PortableExecutablePaths = $portableExecutables
        HelperPortableExecutablePaths = $helperExecutables
        UpdaterLikePortableExecutablePaths = $updaterLikeExecutables
        ExpectedUpdaterPath = $expectedUpdater
    }
}

function Assert-JsonTreeHasUniqueProperties {
    param([System.Text.Json.JsonElement] $Element, [string] $JsonPath)

    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $names = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($property in $Element.EnumerateObject()) {
            Assert-Condition ($names.Add($property.Name)) `
                "Duplicate JSON property in moduleinfo.json at ${JsonPath}: $($property.Name)"
            Assert-JsonTreeHasUniqueProperties $property.Value "$JsonPath.$($property.Name)"
        }
    } elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        $index = 0
        foreach ($item in $Element.EnumerateArray()) {
            Assert-JsonTreeHasUniqueProperties $item "${JsonPath}[$index]"
            ++$index
        }
    }
}

function Get-RequiredJsonProperty {
    param(
        [System.Text.Json.JsonElement] $Object,
        [string] $Name,
        [System.Text.Json.JsonValueKind] $Kind,
        [string] $JsonPath
    )
    Assert-Condition ($Object.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) `
        "Expected a JSON object in moduleinfo.json at $JsonPath."
    foreach ($property in $Object.EnumerateObject()) {
        if ($property.Name -ceq $Name) {
            Assert-Condition ($property.Value.ValueKind -eq $Kind) `
                "moduleinfo.json property '$JsonPath.$Name' has the wrong JSON type."
            return $property.Value
        }
    }
    throw "moduleinfo.json is missing required property '$JsonPath.$Name'."
}

function Assert-ExactJsonString {
    param(
        [System.Text.Json.JsonElement] $Object,
        [string] $Name,
        [string] $Expected,
        [string] $JsonPath
    )
    $value = Get-RequiredJsonProperty $Object $Name ([System.Text.Json.JsonValueKind]::String) $JsonPath
    Assert-Condition ($value.GetString() -ceq $Expected) `
        "moduleinfo.json identity mismatch at '$JsonPath.$Name'."
}

function Test-ModuleInfoContract {
    param(
        [string] $Path,
        [string] $ProductName,
        [string] $Manufacturer,
        [string] $ProductVersion,
        [object[]] $ExpectedClasses
    )

    [byte[]] $bytes = [System.IO.File]::ReadAllBytes($Path)
    Assert-Condition ($bytes.Length -gt 0 -and $bytes.Length -le 1048576) `
        'VST3 moduleinfo.json has an invalid size.'
    $strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
    $jsonText = $strictUtf8.GetString($bytes)
    $options = [System.Text.Json.JsonDocumentOptions]::new()
    $options.AllowTrailingCommas = $true
    $options.CommentHandling = [System.Text.Json.JsonCommentHandling]::Disallow
    $options.MaxDepth = 32
    $document = [System.Text.Json.JsonDocument]::Parse($jsonText, $options)
    try {
        $root = $document.RootElement
        Assert-JsonTreeHasUniqueProperties $root '$'
        Assert-ExactJsonString $root 'Name' $ProductName '$'
        Assert-ExactJsonString $root 'Version' $ProductVersion '$'
        $factoryInfo = Get-RequiredJsonProperty $root 'Factory Info' `
            ([System.Text.Json.JsonValueKind]::Object) '$'
        Assert-ExactJsonString $factoryInfo 'Vendor' $Manufacturer '$.Factory Info'
        $classes = Get-RequiredJsonProperty $root 'Classes' ([System.Text.Json.JsonValueKind]::Array) '$'
        [object[]] $actualClasses = @($classes.EnumerateArray())
        Assert-Condition ($actualClasses.Count -eq $ExpectedClasses.Count) `
            "moduleinfo.json contains $($actualClasses.Count) classes; expected $($ExpectedClasses.Count)."

        $actualIdentities = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($class in $actualClasses) {
            Assert-ExactJsonString $class 'Name' $ProductName '$.Classes[]'
            Assert-ExactJsonString $class 'Vendor' $Manufacturer '$.Classes[]'
            Assert-ExactJsonString $class 'Version' $ProductVersion '$.Classes[]'
            $cidElement = Get-RequiredJsonProperty $class 'CID' ([System.Text.Json.JsonValueKind]::String) '$.Classes[]'
            $categoryElement = Get-RequiredJsonProperty $class 'Category' `
                ([System.Text.Json.JsonValueKind]::String) '$.Classes[]'
            $cid = $cidElement.GetString()
            $category = $categoryElement.GetString()
            Assert-Condition ($cid -cmatch '^[0-9A-F]{32}$') `
                'moduleinfo.json contains a malformed or non-canonical class CID.'
            Assert-Condition ($actualIdentities.Add("$cid|$category")) `
                "moduleinfo.json contains a duplicate class identity: $cid / $category"
        }

        $expectedIdentities = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($expectedClass in $ExpectedClasses) {
            $expectedCid = [string]$expectedClass.cid
            $expectedCategory = [string]$expectedClass.category
            Assert-Condition ($expectedCid -cmatch '^[0-9A-F]{32}$' -and
                              -not [string]::IsNullOrWhiteSpace($expectedCategory) -and
                              $expectedIdentities.Add("$expectedCid|$expectedCategory")) `
                'package-config.json contains a malformed or duplicate VST3 class identity.'
        }
        Assert-Condition ($actualIdentities.SetEquals($expectedIdentities)) `
            'moduleinfo.json class CIDs/categories do not match package-config.json exactly.'
        return [pscustomobject]@{
            Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
            ClassIdentities = @($expectedIdentities | Sort-Object)
        }
    }
    finally {
        $document.Dispose()
    }
}

function Test-PluginVersionResourceContract {
    param(
        [string] $Path,
        [string] $ProductName,
        [string] $Manufacturer,
        [string] $ProductVersion
    )

    $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    Assert-Condition ($versionInfo.FileVersion -ceq $ProductVersion) `
        'Primary VST3 PE FileVersion does not match the requested package version.'
    Assert-Condition ($versionInfo.ProductVersion -ceq $ProductVersion) `
        'Primary VST3 PE ProductVersion does not match the requested package version.'
    Assert-Condition ($versionInfo.CompanyName -ceq $Manufacturer) `
        'Primary VST3 PE CompanyName does not match package-config.json.'
    Assert-Condition ($versionInfo.ProductName -ceq $ProductName) `
        'Primary VST3 PE ProductName does not match package-config.json.'
    Assert-Condition ($versionInfo.FileDescription -ceq $ProductName) `
        'Primary VST3 PE FileDescription does not match package-config.json.'
    return [pscustomobject]@{
        FileVersion = $versionInfo.FileVersion
        ProductVersion = $versionInfo.ProductVersion
        CompanyName = $versionInfo.CompanyName
        ProductName = $versionInfo.ProductName
        FileDescription = $versionInfo.FileDescription
    }
}

function Test-UpdaterVersionResourceContract {
    param(
        [string] $Path,
        [string] $ProductName,
        [string] $Manufacturer,
        [string] $ProductVersion
    )

    $versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    Assert-Condition ($versionInfo.FileVersion -ceq $ProductVersion) `
        'Updater PE FileVersion does not match the requested package version.'
    Assert-Condition ($versionInfo.ProductVersion -ceq $ProductVersion) `
        'Updater PE ProductVersion does not match the requested package version.'
    Assert-Condition ($versionInfo.CompanyName -ceq $Manufacturer) `
        'Updater PE CompanyName does not match package-config.json.'
    Assert-Condition ($versionInfo.ProductName -ceq $ProductName) `
        'Updater PE ProductName does not match package-config.json.'
    Assert-Condition ($versionInfo.FileDescription -ceq "$ProductName Updater") `
        'Updater PE FileDescription does not match package-config.json.'
    Assert-Condition ($versionInfo.InternalName -ceq "$($ProductName)Updater") `
        'Updater PE InternalName does not match package-config.json.'
    Assert-Condition ($versionInfo.OriginalFilename -ceq "$($ProductName)Updater.exe") `
        'Updater PE OriginalFilename does not match package-config.json.'
    return [pscustomobject]@{
        FileVersion = $versionInfo.FileVersion
        ProductVersion = $versionInfo.ProductVersion
        CompanyName = $versionInfo.CompanyName
        ProductName = $versionInfo.ProductName
        FileDescription = $versionInfo.FileDescription
        InternalName = $versionInfo.InternalName
        OriginalFilename = $versionInfo.OriginalFilename
    }
}

function Assert-PolicyMutationRejected {
    param([scriptblock] $Action, [string] $ExpectedMessage, [string] $Description)
    $rejected = $false
    try {
        & $Action | Out-Null
    }
    catch {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*") { throw }
        $rejected = $true
    }
    Assert-Condition $rejected "Installer policy mutation was accepted: $Description"
}

function Write-PolicyTestPe {
    param([string] $Path)
    [byte[]] $bytes = [byte[]]::new(128)
    $bytes[0] = 0x4d
    $bytes[1] = 0x5a
    [byte[]] $offset = [BitConverter]::GetBytes([uint32]64)
    [Buffer]::BlockCopy($offset, 0, $bytes, 0x3c, $offset.Length)
    $bytes[64] = 0x50
    $bytes[65] = 0x45
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path)) | Out-Null
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

function Write-PolicyTestJson {
    param([string] $Path, [object] $Value)
    $json = $Value | ConvertTo-Json -Depth 16
    [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
}

function Invoke-InstallerPolicyMutationTests {
    param([string] $Root)

    $testRoot = Join-Path $Root 'policy-mutation-tests'
    [System.IO.Directory]::CreateDirectory($testRoot) | Out-Null
    try {
        $modulePath = Join-Path $testRoot 'moduleinfo.json'
        $expectedClasses = @(
            [pscustomobject]@{ cid = '11111111111111111111111111111111'; category = 'Audio Module Class' },
            [pscustomobject]@{ cid = '22222222222222222222222222222222'; category = 'Component Controller Class' }
        )
        $validModule = [ordered]@{
            Name = 'PolicyFixture'
            Version = '1.2.3'
            'Factory Info' = [ordered]@{ Vendor = 'Policy Vendor' }
            Classes = @(
                [ordered]@{
                    CID = $expectedClasses[0].cid
                    Category = $expectedClasses[0].category
                    Name = 'PolicyFixture'
                    Vendor = 'Policy Vendor'
                    Version = '1.2.3'
                },
                [ordered]@{
                    CID = $expectedClasses[1].cid
                    Category = $expectedClasses[1].category
                    Name = 'PolicyFixture'
                    Vendor = 'Policy Vendor'
                    Version = '1.2.3'
                }
            )
        }
        $juceTrailingCommaJson = '{"Name":"PolicyFixture","Version":"1.2.3",' +
            '"Factory Info":{"Vendor":"Policy Vendor",},"Classes":[' +
            '{"CID":"11111111111111111111111111111111","Category":"Audio Module Class",' +
            '"Name":"PolicyFixture","Vendor":"Policy Vendor","Version":"1.2.3",},' +
            '{"CID":"22222222222222222222222222222222","Category":"Component Controller Class",' +
            '"Name":"PolicyFixture","Vendor":"Policy Vendor","Version":"1.2.3",},],}'
        [System.IO.File]::WriteAllText($modulePath, $juceTrailingCommaJson + [Environment]::NewLine,
            [System.Text.UTF8Encoding]::new($false))
        Test-ModuleInfoContract $modulePath 'PolicyFixture' 'Policy Vendor' '1.2.3' $expectedClasses | Out-Null

        Write-PolicyTestJson $modulePath $validModule
        Test-ModuleInfoContract $modulePath 'PolicyFixture' 'Policy Vendor' '1.2.3' $expectedClasses | Out-Null

        $identityMutations = @(
            [pscustomobject]@{ Description = 'product'; Expected = 'identity mismatch'; Apply = {
                param($value) $value.Name = 'WrongProduct'
            } },
            [pscustomobject]@{ Description = 'manufacturer'; Expected = 'identity mismatch'; Apply = {
                param($value) $value.'Factory Info'.Vendor = 'Wrong Vendor'
            } },
            [pscustomobject]@{ Description = 'version'; Expected = 'identity mismatch'; Apply = {
                param($value) $value.Version = '9.9.9'
            } },
            [pscustomobject]@{ Description = 'class CID'; Expected = 'do not match'; Apply = {
                param($value) $value.Classes[0].CID = 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
            } },
            [pscustomobject]@{ Description = 'class category'; Expected = 'do not match'; Apply = {
                param($value) $value.Classes[0].Category = 'Service Class'
            } },
            [pscustomobject]@{ Description = 'extra class'; Expected = 'classes; expected'; Apply = {
                param($value) $value.Classes += $value.Classes[0]
            } }
        )
        foreach ($mutation in $identityMutations) {
            $mutated = ($validModule | ConvertTo-Json -Depth 16) | ConvertFrom-Json
            & $mutation.Apply $mutated
            Write-PolicyTestJson $modulePath $mutated
            Assert-PolicyMutationRejected {
                Test-ModuleInfoContract $modulePath 'PolicyFixture' 'Policy Vendor' '1.2.3' $expectedClasses
            } $mutation.Expected "moduleinfo $($mutation.Description)"
        }

        $payloadRoot = Join-Path $testRoot 'PolicyFixture.vst3'
        $pluginRelative = 'Contents\x86_64-win\PolicyFixture.vst3'
        $pluginPath = Join-Path $payloadRoot $pluginRelative
        Write-PolicyTestPe $pluginPath
        $renamedPeRelative = 'Contents\Resources\opaque-resource.dat'
        $renamedPePath = Join-Path $payloadRoot $renamedPeRelative
        Write-PolicyTestPe $renamedPePath
        $renamedPeClassification = Get-PeImageClassification $renamedPePath $renamedPeRelative
        Assert-Condition ($renamedPeClassification.IsPortableExecutable) `
            'Content-based PE classifier missed a valid PE with an opaque extension.'
        Assert-PolicyMutationRejected {
            Get-PayloadExecutableContract (Get-SafeTreeSnapshot $payloadRoot) `
                'PolicyFixture' $pluginRelative $true
        } 'only the exact primary VST3 PE' 'unsigned renamed PE helper'
        [System.IO.File]::Delete($renamedPePath)

        $fakeExe = Join-Path $payloadRoot 'Contents\Resources\fake.exe'
        [System.IO.File]::WriteAllText($fakeExe, 'not an executable')
        Assert-PolicyMutationRejected {
            Get-PayloadExecutableContract (Get-SafeTreeSnapshot $payloadRoot) `
                'PolicyFixture' $pluginRelative $true
        } 'does not contain an MZ/PE image' 'non-MZ .exe'
        [System.IO.File]::Delete($fakeExe)

        $malformedMz = Join-Path $payloadRoot 'Contents\Resources\malformed.dat'
        [byte[]] $malformedMzBytes = [byte[]]::new(128)
        $malformedMzBytes[0] = 0x4d
        $malformedMzBytes[1] = 0x5a
        [System.IO.File]::WriteAllBytes($malformedMz, $malformedMzBytes)
        Assert-PolicyMutationRejected {
            Get-PayloadExecutableContract (Get-SafeTreeSnapshot $payloadRoot) `
                'PolicyFixture' $pluginRelative $true
        } 'invalid PE header offset' 'malformed MZ image'
        [System.IO.File]::Delete($malformedMz)

        $scriptFile = Join-Path $payloadRoot 'Contents\Resources\payload.ps1'
        [System.IO.File]::WriteAllText($scriptFile, 'exit 0')
        Assert-PolicyMutationRejected {
            Get-PayloadExecutableContract (Get-SafeTreeSnapshot $payloadRoot) `
                'PolicyFixture' $pluginRelative $true
        } 'dangerous executable extension' 'embedded script'
        [System.IO.File]::Delete($scriptFile)

        $hiddenHelper = Join-Path $payloadRoot 'Contents\Helpers\worker.bin'
        Write-PolicyTestPe $hiddenHelper
        Assert-PolicyMutationRejected {
            Get-PayloadExecutableContract (Get-SafeTreeSnapshot $payloadRoot) `
                'PolicyFixture' $pluginRelative $true
        } 'must not embed an updater or any additional PE helper' 'unsigned hidden PE helper'
        [System.IO.File]::Delete($hiddenHelper)

        $expectedUpdater = Join-Path $payloadRoot 'Contents\Helpers\PolicyFixtureUpdater.exe'
        $secondUpdater = Join-Path $payloadRoot 'Contents\Resources\EmergencyUpdate.bin'
        Write-PolicyTestPe $expectedUpdater
        Write-PolicyTestPe $secondUpdater
        Assert-PolicyMutationRejected {
            Get-PayloadExecutableContract (Get-SafeTreeSnapshot $payloadRoot) `
                'PolicyFixture' $pluginRelative $false
        } 'exactly one updater-like PE helper' 'second updater-like PE'

        return 12
    }
    finally {
        if (Test-Path -LiteralPath $testRoot -PathType Container) {
            Remove-Item -LiteralPath $testRoot -Recurse -Force
        }
    }
}

function Normalize-Guid {
    param([string] $Value)
    return ([guid]::Parse($Value)).ToString('D').ToUpperInvariant()
}

function Convert-GuidToNetworkBytes {
    param([guid] $Guid)
    [byte[]] $bytes = $Guid.ToByteArray()
    [Array]::Reverse($bytes, 0, 4)
    [Array]::Reverse($bytes, 4, 2)
    [Array]::Reverse($bytes, 6, 2)
    return $bytes
}

function New-NameBasedGuid {
    param([guid] $Namespace, [string] $Name)
    [byte[]] $namespaceBytes = Convert-GuidToNetworkBytes $Namespace
    [byte[]] $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($Name)
    [byte[]] $inputBytes = [byte[]]::new($namespaceBytes.Length + $nameBytes.Length)
    [Buffer]::BlockCopy($namespaceBytes, 0, $inputBytes, 0, $namespaceBytes.Length)
    [Buffer]::BlockCopy($nameBytes, 0, $inputBytes, $namespaceBytes.Length, $nameBytes.Length)
    [byte[]] $digest = [System.Security.Cryptography.SHA1]::HashData($inputBytes)
    [byte[]] $guidBytes = $digest[0..15]
    $guidBytes[6] = ($guidBytes[6] -band 0x0f) -bor 0x50
    $guidBytes[8] = ($guidBytes[8] -band 0x3f) -bor 0x80
    [Array]::Reverse($guidBytes, 0, 4)
    [Array]::Reverse($guidBytes, 4, 2)
    [Array]::Reverse($guidBytes, 6, 2)
    return ([guid]::new($guidBytes)).ToString('D').ToUpperInvariant()
}

function Invoke-Wix {
    param([string[]] $Arguments)
    Push-Location $script:RepoRoot
    try {
        $output = @(& dotnet tool run wix -- @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
        $output | ForEach-Object { Write-Host $_ }
        if ($exitCode -ne 0) {
            throw "WiX failed with exit code $exitCode: $($Arguments -join ' ')"
        }
        return $output
    }
    finally {
        Pop-Location
    }
}

function Restore-PinnedWix {
    foreach ($required in @($script:ToolManifest, $script:NuGetConfig, $script:PackageSource, $script:PackageConfig)) {
        Assert-Condition (Test-Path -LiteralPath $required -PathType Leaf) "Required installer input is missing: $required"
    }
    $manifest = Get-Content -LiteralPath $script:ToolManifest -Raw | ConvertFrom-Json
    $toolProperties = @($manifest.tools.PSObject.Properties)
    Assert-Condition ($manifest.version -eq 1 -and $manifest.isRoot -eq $true) 'Unexpected .NET tool manifest schema.'
    Assert-Condition ($toolProperties.Count -eq 1 -and $toolProperties[0].Name -ceq 'wix') `
        'The local tool manifest must contain only WiX.'
    Assert-Condition ($manifest.tools.wix.version -ceq $script:RequiredWixVersion) `
        "WiX must be pinned to $script:RequiredWixVersion."
    Assert-Condition (@($manifest.tools.wix.commands).Count -eq 1 -and
                      $manifest.tools.wix.commands[0] -ceq 'wix') 'Unexpected WiX tool command.'

    [xml] $nuget = Get-Content -LiteralPath $script:NuGetConfig -Raw
    $sources = @($nuget.SelectNodes('/configuration/packageSources/add'))
    $sourceClears = @($nuget.SelectNodes('/configuration/packageSources/clear'))
    Assert-Condition ($sourceClears.Count -eq 1 -and
                      $sources.Count -eq 1 -and $sources[0].GetAttribute('key') -ceq 'nuget.org' -and
                      $sources[0].GetAttribute('value') -ceq 'https://api.nuget.org/v3/index.json') `
        'NuGet.Config must contain only the canonical nuget.org source.'
    Assert-Condition ($null -ne $nuget.SelectSingleNode(
        '/configuration/config/add[@key="signatureValidationMode" and @value="require"]')) `
        'NuGet signature validation must be required.'
    $trustedSigners = @($nuget.SelectNodes('/configuration/trustedSigners/*'))
    $trustedCertificates = @($nuget.SelectNodes('/configuration/trustedSigners/author/certificate'))
    Assert-Condition ($trustedSigners.Count -eq 1 -and $trustedSigners[0].Name -ceq 'author' -and
                      $trustedSigners[0].GetAttribute('name') -ceq 'firegiant' -and
                      $trustedCertificates.Count -eq 1 -and
                      $trustedCertificates[0].GetAttribute('fingerprint') -ceq
                          $script:RequiredNuGetSignerFingerprint -and
                      $trustedCertificates[0].GetAttribute('hashAlgorithm') -ceq 'SHA256' -and
                      $trustedCertificates[0].GetAttribute('allowUntrustedRoot') -ceq 'false') `
        'NuGet.Config does not contain the required FireGiant signing certificate.'

    $dotnet = Get-Command dotnet -ErrorAction Stop
    Push-Location $script:RepoRoot
    try {
        & $dotnet.Source tool restore --tool-manifest $script:ToolManifest `
            --configfile $script:NuGetConfig --disable-parallel --no-cache
        if ($LASTEXITCODE -ne 0) { throw "Pinned WiX tool restore failed with exit code $LASTEXITCODE." }
    }
    finally {
        Pop-Location
    }

    $reported = ((Invoke-Wix @('--version') | Out-String).Trim())
    Assert-Condition ($reported -match '^6\.0\.2(?:\+\S+)?$') `
        "Restored WiX reported '$reported', expected $script:RequiredWixVersion."
}

function Resolve-Dumpbin {
    param([string] $RequestedPath)
    if ($RequestedPath) { return Resolve-ExistingFile $RequestedPath 'dumpbin.exe' }

    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    Assert-Condition ($programFilesX86) 'ProgramFiles(x86) is unavailable; pass -DumpbinPath explicitly.'
    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vswhere = Resolve-ExistingFile $vswhere 'vswhere.exe'
    $installation = ((& $vswhere -latest -products '*' -property installationPath) | Out-String).Trim()
    Assert-Condition ($LASTEXITCODE -eq 0 -and $installation) 'Visual Studio could not be resolved with vswhere.'
    $versionFile = Join-Path $installation 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    $versionFile = Resolve-ExistingFile $versionFile 'Visual C++ default toolset version file'
    $toolVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    $nativeArm = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -eq
                 [System.Runtime.InteropServices.Architecture]::Arm64
    $hostDirectory = if ($nativeArm) { 'Hostarm64' } else { 'Hostx64' }
    $targetDirectory = if ($nativeArm) { 'arm64' } else { 'x64' }
    $resolved = Join-Path $installation "VC\Tools\MSVC\$toolVersion\bin\$hostDirectory\$targetDirectory\dumpbin.exe"
    return Resolve-ExistingFile $resolved 'native dumpbin.exe'
}

function Resolve-SignTool {
    param([string] $RequestedPath)
    if ($RequestedPath) { return Resolve-ExistingFile $RequestedPath 'signtool.exe' }

    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    Assert-Condition ($programFilesX86) 'ProgramFiles(x86) is unavailable; pass -SignToolPath explicitly.'
    $sdkBin = Join-Path $programFilesX86 'Windows Kits\10\bin'
    Assert-Condition (Test-Path -LiteralPath $sdkBin -PathType Container) `
        'Windows SDK bin directory is unavailable; pass -SignToolPath explicitly.'
    $versions = @(Get-ChildItem -LiteralPath $sdkBin -Directory | Where-Object {
        $parsed = [version]'0.0'
        [version]::TryParse($_.Name, [ref] $parsed)
    } | Sort-Object { [version] $_.Name } -Descending)
    $nativeArm = [System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -eq
                 [System.Runtime.InteropServices.Architecture]::Arm64
    $toolArchitectures = if ($nativeArm) { @('arm64', 'x64') } else { @('x64') }
    foreach ($versionDirectory in $versions) {
        foreach ($toolArchitecture in $toolArchitectures) {
            $candidate = Join-Path $versionDirectory.FullName "$toolArchitecture\signtool.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return Resolve-ExistingFile $candidate 'signtool.exe'
            }
        }
    }
    throw 'signtool.exe was not found in the Windows SDK; pass -SignToolPath explicitly.'
}

function Resolve-SigningCertificate {
    param(
        [string] $Thumbprint,
        [string] $Subject,
        [string] $StoreName,
        [bool] $MachineStore
    )

    Assert-Condition ($StoreName -match '^[A-Za-z0-9._-]+$') 'Unsafe CertificateStoreName.'
    $storeLocation = if ($MachineStore) { 'LocalMachine' } else { 'CurrentUser' }
    $storePath = "Cert:\$storeLocation\$StoreName"
    Assert-Condition (Test-Path -LiteralPath $storePath -PathType Container) `
        "Certificate store was not found: $storePath"
    $certificates = @(Get-ChildItem -LiteralPath $storePath | Where-Object {
        $_ -is [System.Security.Cryptography.X509Certificates.X509Certificate2]
    })

    if ($Thumbprint) {
        $requestedThumbprint = $Thumbprint.Replace(' ', '').ToUpperInvariant()
        Assert-Condition ($requestedThumbprint -match '^[0-9A-F]{40}$') `
            'CertificateThumbprint must be 40 hexadecimal SHA-1 characters.'
        $matches = @($certificates | Where-Object {
            $_.Thumbprint.Replace(' ', '').ToUpperInvariant() -ceq $requestedThumbprint
        })
    } else {
        $matches = @($certificates | Where-Object { ([string] $_.Subject) -ceq $Subject })
    }

    Assert-Condition ($matches.Count -eq 1) `
        'Signing identity must resolve to exactly one certificate in the selected store.'
    $certificate = $matches[0]
    Assert-Condition ($certificate.HasPrivateKey) 'Signing certificate has no accessible private key.'
    $now = [DateTime]::UtcNow
    Assert-Condition ($certificate.NotBefore.ToUniversalTime() -le $now -and
                      $certificate.NotAfter.ToUniversalTime() -ge $now) `
        'Signing certificate is outside its validity period.'
    $resolvedThumbprint = $certificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
    $sha256 = $certificate.GetCertHashString(
        [System.Security.Cryptography.HashAlgorithmName]::SHA256).ToUpperInvariant()
    [string[]] $certificateArguments = @('/sha1', $resolvedThumbprint, '/s', $StoreName)
    if ($MachineStore) { $certificateArguments += '/sm' }
    return [pscustomobject]@{
        Thumbprint = $resolvedThumbprint
        Sha256 = $sha256
        Arguments = $certificateArguments
    }
}

function Assert-PeArchitecture {
    param([string] $Path, [string] $ExpectedArchitecture, [string] $Dumpbin)
    $headers = @(& $Dumpbin /headers $Path 2>&1)
    $exitCode = $LASTEXITCODE
    $text = $headers -join [Environment]::NewLine
    Assert-Condition ($exitCode -eq 0) "dumpbin failed for '$Path' with exit code $exitCode.`n$text"
    $expected = if ($ExpectedArchitecture -ceq 'arm64ec') {
        '(?m)^\s*8664 machine \(x64\) \(ARM64X\)\s*$'
    } else {
        '(?m)^\s*8664 machine \(x64\)\s*$'
    }
    Assert-Condition ($text -match $expected) "Unexpected $ExpectedArchitecture PE image: $Path`n$text"
}

function Invoke-AuthenticodeSign {
    param(
        [string] $Path,
        [string] $SignTool,
        [string[]] $CertificateArguments,
        [string] $ExpectedSignerThumbprint,
        [uri] $Timestamp
    )
    $arguments = @('sign', '/fd', 'SHA256', '/td', 'SHA256', '/tr', $Timestamp.AbsoluteUri) +
                 $CertificateArguments + @($Path)
    $signOutput = @(& $SignTool @arguments 2>&1)
    $signExitCode = $LASTEXITCODE
    $signOutput | ForEach-Object { Write-Host $_ }
    Assert-Condition ($signExitCode -eq 0) "Authenticode signing failed: $Path"
    $verifyOutput = @(& $SignTool verify /pa /all /v $Path 2>&1)
    $verifyExitCode = $LASTEXITCODE
    $verifyOutput | ForEach-Object { Write-Host $_ }
    Assert-Condition ($verifyExitCode -eq 0) "Authenticode verification failed: $Path"

    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    Assert-Condition ($null -ne $signature.SignerCertificate) `
        "Signed file has no inspectable signer certificate: $Path"
    Assert-Condition ($null -ne $signature.TimeStamperCertificate) `
        "Signed file has no inspectable RFC3161 timestamp certificate: $Path"
    $actualThumbprint = $signature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
    Assert-Condition ($actualThumbprint -ceq $ExpectedSignerThumbprint) `
        "Unexpected signer certificate after signing: $Path"
    $actualSha256 = $signature.SignerCertificate.GetCertHashString(
        [System.Security.Cryptography.HashAlgorithmName]::SHA256).ToUpperInvariant()
    return [pscustomobject]@{ SignerSha256 = $actualSha256 }
}

function Invoke-ComMethod {
    param([object] $Object, [string] $Name, [object[]] $Arguments = @())
    return $Object.GetType().InvokeMember(
        $Name, [System.Reflection.BindingFlags]::InvokeMethod, $null, $Object, $Arguments)
}

function Get-ComProperty {
    param([object] $Object, [string] $Name, [object[]] $Arguments = @())
    return $Object.GetType().InvokeMember(
        $Name, [System.Reflection.BindingFlags]::GetProperty, $null, $Object, $Arguments)
}

function Get-MsiRows {
    param([object] $Database, [string] $Query)
    $view = Invoke-ComMethod $Database 'OpenView' @($Query)
    $rows = [System.Collections.Generic.List[object]]::new()
    try {
        Invoke-ComMethod $view 'Execute' | Out-Null
        while ($true) {
            $record = Invoke-ComMethod $view 'Fetch'
            if ($null -eq $record) { break }
            try {
                $fieldCount = [int](Get-ComProperty $record 'FieldCount')
                [string[]] $fields = for ($index = 1; $index -le $fieldCount; ++$index) {
                    [string](Get-ComProperty $record 'StringData' @($index))
                }
                $rows.Add([pscustomobject]@{ Fields = $fields })
            }
            finally {
                [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
            }
        }
    }
    finally {
        Invoke-ComMethod $view 'Close' | Out-Null
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    }
    return $rows.ToArray()
}

function Get-MsiSummaryProperty {
    param([object] $Installer, [string] $MsiPath, [int] $PropertyId)
    $summary = Get-ComProperty $Installer 'SummaryInformation' @($MsiPath, 0)
    try { return [string](Get-ComProperty $summary 'Property' @($PropertyId)) }
    finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($summary) }
}

function Assert-MsiUpgradeRow {
    param(
        [object] $Row,
        [string] $ExpectedUpgradeCode,
        [string] $ExpectedVersionMin,
        [string] $ExpectedVersionMax,
        [string] $ExpectedLanguage,
        [int] $ExpectedAttributes,
        [string] $ExpectedActionProperty
    )
    Assert-Condition ($Row.Fields.Count -eq 7) "Malformed Upgrade row: $ExpectedActionProperty"
    Assert-Condition ((Normalize-Guid $Row.Fields[0]) -ceq $ExpectedUpgradeCode) `
        "UpgradeCode mismatch for $ExpectedActionProperty."
    Assert-Condition ($Row.Fields[1] -ceq $ExpectedVersionMin -and
                      $Row.Fields[2] -ceq $ExpectedVersionMax -and
                      $Row.Fields[3] -ceq $ExpectedLanguage -and
                      [int]($Row.Fields[4]) -eq $ExpectedAttributes -and
                      $Row.Fields[5] -ceq '' -and
                      $Row.Fields[6] -ceq $ExpectedActionProperty) `
        "Upgrade row semantics mismatch for $ExpectedActionProperty."
}

function Assert-MsiDirectoryContract {
    param([object] $Directories, [string[]] $ComponentDirectories)

    Assert-Condition ($Directories.ContainsKey('TARGETDIR')) 'MSI Directory graph has no TARGETDIR root.'
    Assert-Condition ($Directories.ContainsKey('INSTALLFOLDER')) `
        'MSI Directory graph has no INSTALLFOLDER.'
    Assert-Condition (([string]$Directories['TARGETDIR'][0]) -ceq '') `
        'MSI TARGETDIR must be the only root of the Directory graph.'

    foreach ($identifier in @($Directories.Keys)) {
        $current = [string]$identifier
        $visited = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        while ($current) {
            Assert-Condition ($visited.Add($current)) `
                "MSI Directory graph contains a cycle at '$current'."
            Assert-Condition ($Directories.ContainsKey($current)) `
                "MSI Directory graph contains a dangling parent '$current'."
            $parent = [string]$Directories[$current][0]
            if ($current -ceq 'TARGETDIR') {
                Assert-Condition ($parent -ceq '') 'MSI TARGETDIR has an unexpected parent.'
                $current = ''
            } else {
                Assert-Condition (-not [string]::IsNullOrWhiteSpace($parent)) `
                    "MSI Directory '$current' does not descend from TARGETDIR."
                $current = $parent
            }
        }
    }

    Assert-Condition ($ComponentDirectories.Count -gt 0) 'MSI has no payload components.'
    foreach ($componentDirectory in $ComponentDirectories) {
        $current = [string]$componentDirectory
        $visited = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        while ($current -cne 'INSTALLFOLDER') {
            Assert-Condition (-not [string]::IsNullOrWhiteSpace($current)) `
                "MSI component directory '$componentDirectory' does not descend from INSTALLFOLDER."
            Assert-Condition ($visited.Add($current)) `
                "MSI component directory path contains a cycle at '$current'."
            Assert-Condition ($Directories.ContainsKey($current)) `
                "MSI component directory refers to unknown Directory '$current'."
            $current = [string]$Directories[$current][0]
        }
    }
}

function Test-MsiContract {
    param(
        [string] $MsiPath,
        [string] $ProductName,
        [string] $DisplayName,
        [string] $Manufacturer,
        [string] $ProductVersion,
        [string] $ExpectedProductCode,
        [string] $UpgradeCode,
        [string] $OtherUpgradeCode,
        [string] $MsiArchitecture,
        [int] $PayloadFileCount
    )

    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $null
    try {
        $database = Invoke-ComMethod $installer 'OpenDatabase' @($MsiPath, 0)
        $tableRows = @(Get-MsiRows $database 'SELECT `Name` FROM `_Tables`')
        $tables = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($row in $tableRows) { [void]$tables.Add($row.Fields[0]) }
        foreach ($requiredTable in @('Property', 'Directory', 'Component', 'File', 'Media',
                                      'Upgrade', 'LaunchCondition', 'InstallExecuteSequence')) {
            Assert-Condition ($tables.Contains($requiredTable)) "MSI table is missing: $requiredTable"
        }
        $forbiddenSideEffectTables = @(
            'CustomAction', 'Binary', 'ServiceInstall', 'ServiceControl',
            'Registry', 'RemoveRegistry', 'SelfReg', 'TypeLib', 'Class', 'ProgId',
            'Extension', 'MIME', 'AppId', 'ODBCDataSource', 'ODBCDriver',
            'ODBCTranslator', 'IniFile', 'RemoveIniFile', 'Environment', 'RemoveFile',
            'MoveFiles', 'DuplicateFile', 'CreateFolder', 'Shortcut', 'ReserveCost',
            'BindImage', 'Font', 'IsolatedComponent', 'MsiAssembly', 'MsiAssemblyName',
            'PublishComponent', 'Complus', 'Verb', 'ODBCAttribute', 'LockPermissions',
            'MsiLockPermissionsEx', 'Permission', 'PermissionEx', 'Patch', 'PatchPackage',
            'SFPCatalog'
        )
        foreach ($forbiddenTable in $forbiddenSideEffectTables) {
            Assert-Condition (-not $tables.Contains($forbiddenTable)) `
                "Forbidden MSI side-effect table is present: $forbiddenTable"
        }

        $properties = [System.Collections.Generic.Dictionary[string, string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
        foreach ($row in @(Get-MsiRows $database 'SELECT `Property`, `Value` FROM `Property`')) {
            Assert-Condition ($row.Fields.Count -eq 2 -and $row.Fields[0] -and
                              -not $properties.ContainsKey($row.Fields[0])) `
                'Malformed or duplicate MSI Property row.'
            $properties.Add($row.Fields[0], $row.Fields[1])
        }
        foreach ($requiredProperty in @('ProductName', 'Manufacturer', 'ProductVersion',
                                         'ProductCode', 'UpgradeCode', 'ALLUSERS',
                                         'ProductLanguage', 'MSIDEPLOYMENTCOMPLIANT')) {
            Assert-Condition ($properties.ContainsKey($requiredProperty)) `
                "MSI property is missing: $requiredProperty"
        }
        Assert-Condition ($properties['ProductName'] -ceq $DisplayName) 'MSI ProductName mismatch.'
        Assert-Condition ($properties['Manufacturer'] -ceq $Manufacturer) 'MSI Manufacturer mismatch.'
        Assert-Condition ($properties['ProductVersion'] -ceq $ProductVersion) 'MSI ProductVersion mismatch.'
        Assert-Condition ((Normalize-Guid $properties['ProductCode']) -ceq $ExpectedProductCode) 'MSI ProductCode mismatch.'
        Assert-Condition ((Normalize-Guid $properties['UpgradeCode']) -ceq $UpgradeCode) 'MSI UpgradeCode mismatch.'
        Assert-Condition ($properties['ALLUSERS'] -ceq '1') 'MSI is not per-machine (ALLUSERS=1).'
        Assert-Condition ($properties['ProductLanguage'] -ceq '1033') 'MSI ProductLanguage mismatch.'
        Assert-Condition ($properties['MSIDEPLOYMENTCOMPLIANT'] -ceq '1') `
            'MSI is not explicitly marked as UAC deployment compliant.'
        Assert-Condition ($properties.ContainsKey('SecureCustomProperties')) `
            'MSI SecureCustomProperties is missing.'
        $secureCustomProperties = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($secureProperty in $properties['SecureCustomProperties'].Split(';')) {
            Assert-Condition ($secureProperty -and $secureCustomProperties.Add($secureProperty)) `
                'MSI SecureCustomProperties contains an empty or duplicate entry.'
        }
        $expectedSecureCustomProperties = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($secureProperty in @('WIX_UPGRADE_DETECTED', 'WIX_DOWNGRADE_DETECTED',
                                       'OTHERARCHITECTUREDETECTED')) {
            [void]$expectedSecureCustomProperties.Add($secureProperty)
        }
        Assert-Condition ($secureCustomProperties.SetEquals($expectedSecureCustomProperties)) `
            'MSI SecureCustomProperties is not the exact upgrade-detection set.'

        $template = Get-MsiSummaryProperty $installer $MsiPath 7
        $templatePlatform = $template.Split(';')[0]
        if ($MsiArchitecture -ceq 'arm64') {
            Assert-Condition ($templatePlatform -ieq 'Arm64') "Expected Arm64 MSI template, found '$template'."
        } else {
            Assert-Condition ($templatePlatform -in @('x64', 'Intel64')) "Expected x64 MSI template, found '$template'."
        }

        $directories = [System.Collections.Generic.Dictionary[string, object]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($row in @(Get-MsiRows $database 'SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`')) {
            Assert-Condition ($row.Fields.Count -eq 3 -and $row.Fields[0] -and
                              -not $directories.ContainsKey($row.Fields[0])) `
                'Malformed or duplicate MSI Directory row.'
            $directories.Add($row.Fields[0], @($row.Fields[1], $row.Fields[2]))
        }
        Assert-Condition ($directories.ContainsKey('VST3Folder')) 'MSI VST3Folder is missing.'
        Assert-Condition ($directories.ContainsKey('INSTALLFOLDER')) 'MSI INSTALLFOLDER is missing.'
        $vst3Parent = $directories['VST3Folder'][0]
        Assert-Condition ($vst3Parent -in @('CommonFiles6432Folder', 'CommonFiles64Folder')) `
            "VST3Folder is not rooted in 64-bit Common Files: $vst3Parent"
        Assert-Condition ($directories['VST3Folder'][1].Split('|')[-1] -ceq 'VST3') 'MSI VST3 directory mismatch.'
        Assert-Condition ($directories['INSTALLFOLDER'][0] -ceq 'VST3Folder') 'MSI bundle parent mismatch.'
        Assert-Condition ($directories['INSTALLFOLDER'][1].Split('|')[-1] -ceq "$ProductName.vst3") `
            'MSI bundle directory mismatch.'

        $componentIds = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        $componentDirectories = [System.Collections.Generic.List[string]]::new()
        $componentRows = @(Get-MsiRows $database `
            'SELECT `Component`, `Directory_`, `Attributes` FROM `Component`')
        Assert-Condition ($componentRows.Count -gt 0) 'MSI has no components.'
        foreach ($row in $componentRows) {
            Assert-Condition ($row.Fields.Count -eq 3 -and $row.Fields[0] -and
                              $componentIds.Add($row.Fields[0])) `
                'Malformed or duplicate MSI Component row.'
            Assert-Condition (([int]($row.Fields[2]) -band 256) -ne 0) `
                'MSI contains a non-64-bit component.'
            $componentDirectories.Add($row.Fields[1])
        }
        Assert-MsiDirectoryContract $directories ($componentDirectories.ToArray())

        $fileRows = @(Get-MsiRows $database 'SELECT `File`, `Component_` FROM `File`')
        Assert-Condition ($fileRows.Count -eq $PayloadFileCount) `
            "MSI File table has $($fileRows.Count) rows; expected $PayloadFileCount."
        foreach ($row in $fileRows) {
            Assert-Condition ($row.Fields.Count -eq 2 -and $row.Fields[0] -and
                              $componentIds.Contains($row.Fields[1])) `
                'MSI File row refers to an unknown payload component.'
        }
        foreach ($row in @(Get-MsiRows $database 'SELECT `Cabinet` FROM `Media`')) {
            Assert-Condition ($row.Fields[0].StartsWith('#')) 'MSI payload cabinet is not embedded.'
        }

        $upgradeRows = @(Get-MsiRows $database `
            'SELECT `UpgradeCode`, `VersionMin`, `VersionMax`, `Language`, `Attributes`, `Remove`, `ActionProperty` FROM `Upgrade`')
        Assert-Condition ($upgradeRows.Count -eq 3) "MSI has $($upgradeRows.Count) Upgrade rows; expected exactly 3."
        $upgradeByProperty = @{}
        foreach ($row in $upgradeRows) {
            $actionProperty = $row.Fields[6]
            Assert-Condition (-not $upgradeByProperty.ContainsKey($actionProperty)) `
                "Duplicate Upgrade row property: $actionProperty"
            $upgradeByProperty[$actionProperty] = $row
        }
        foreach ($requiredProperty in @('WIX_UPGRADE_DETECTED', 'WIX_DOWNGRADE_DETECTED',
                                         'OTHERARCHITECTUREDETECTED')) {
            Assert-Condition ($upgradeByProperty.ContainsKey($requiredProperty)) `
                "Upgrade row is missing: $requiredProperty"
        }
        Assert-MsiUpgradeRow $upgradeByProperty['WIX_UPGRADE_DETECTED'] $UpgradeCode '' `
            $ProductVersion '1033' 1 'WIX_UPGRADE_DETECTED'
        Assert-MsiUpgradeRow $upgradeByProperty['WIX_DOWNGRADE_DETECTED'] $UpgradeCode `
            $ProductVersion '' '1033' 2 'WIX_DOWNGRADE_DETECTED'
        Assert-MsiUpgradeRow $upgradeByProperty['OTHERARCHITECTUREDETECTED'] $OtherUpgradeCode `
            '0.0.0' '' '' 258 'OTHERARCHITECTUREDETECTED'
        $launchConditions = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        foreach ($row in @(Get-MsiRows $database 'SELECT `Condition` FROM `LaunchCondition`')) {
            Assert-Condition ($row.Fields.Count -eq 1 -and $row.Fields[0]) `
                'Malformed MSI LaunchCondition row.'
            $normalized = $row.Fields[0].ToUpperInvariant() -replace '[ \t()]', ''
            Assert-Condition ($launchConditions.Add($normalized)) `
                'Duplicate MSI LaunchCondition row.'
        }
        $expectedLaunchConditions = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::Ordinal)
        [void]$expectedLaunchConditions.Add('INSTALLEDORNOTWIX_DOWNGRADE_DETECTED')
        [void]$expectedLaunchConditions.Add('INSTALLEDORNOTOTHERARCHITECTUREDETECTED')
        Assert-Condition ($launchConditions.SetEquals($expectedLaunchConditions)) `
            'MSI LaunchCondition table is not the exact downgrade/architecture contract.'

        $sequences = @{}
        foreach ($row in @(Get-MsiRows $database 'SELECT `Action`, `Sequence` FROM `InstallExecuteSequence`')) {
            $sequences[$row.Fields[0]] = [int]($row.Fields[1])
        }
        foreach ($action in @('InstallInitialize', 'RemoveExistingProducts', 'InstallFiles')) {
            Assert-Condition ($sequences.ContainsKey($action)) "MSI sequence action is missing: $action"
        }
        Assert-Condition ($sequences['RemoveExistingProducts'] -gt $sequences['InstallInitialize'] -and
                          $sequences['RemoveExistingProducts'] -lt $sequences['InstallFiles']) `
            'RemoveExistingProducts is not rollback-safe after InstallInitialize and before InstallFiles.'

        $forbiddenSequenceActions = @('ForceReboot', 'ScheduleReboot', 'DisableRollback')
        foreach ($sequenceTable in @('InstallExecuteSequence', 'InstallUISequence',
                                      'AdminExecuteSequence', 'AdminUISequence',
                                      'AdvtExecuteSequence')) {
            if (-not $tables.Contains($sequenceTable)) { continue }
            $query = 'SELECT `Action` FROM `{0}`' -f $sequenceTable
            foreach ($row in @(Get-MsiRows $database $query)) {
                Assert-Condition ($row.Fields.Count -eq 1 -and
                                  $forbiddenSequenceActions -cnotcontains $row.Fields[0]) `
                    "MSI contains a forbidden reboot/rollback action in ${sequenceTable}: $($row.Fields[0])"
            }
        }

        return [pscustomobject]@{
            ProductCode = Normalize-Guid $properties['ProductCode']
            Template = $template
            FileRows = $fileRows.Count
            ComponentRows = $componentRows.Count
            RemoveExistingProductsSequence = $sequences['RemoveExistingProducts']
            ProductName = $properties['ProductName']
            Manufacturer = $properties['Manufacturer']
            ProductLanguage = $properties['ProductLanguage']
            DeploymentCompliant = $properties['MSIDEPLOYMENTCOMPLIANT']
            SecureCustomProperties = @($secureCustomProperties | Sort-Object)
            LaunchConditions = @($launchConditions | Sort-Object)
        }
    }
    finally {
        if ($null -ne $database) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
    }
}

function Stop-TimedOutProcess {
    param(
        [System.Diagnostics.Process] $Process,
        [string] $Description,
        [int] $TimeoutSeconds
    )
    $killFailure = $null
    try {
        $Process.Kill($true)
        $Process.WaitForExit()
    }
    catch {
        $killFailure = $_.Exception.Message
    }
    finally {
        $Process.Dispose()
    }
    $suffix = if ($killFailure) { " Process-tree termination also failed: $killFailure" } else { '' }
    throw "$Description timed out after $TimeoutSeconds seconds.$suffix"
}

function New-CryptographicHex {
    param(
        [Parameter(Mandatory)]
        [ValidateRange(1, 1024)]
        [int] $ByteCount
    )
    [byte[]] $bytes = [byte[]]::new($ByteCount)
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
    return [Convert]::ToHexString($bytes)
}

function Assert-NamedPipeClientProcess {
    param(
        [System.IO.Pipes.NamedPipeServerStream] $Pipe,
        [System.Diagnostics.Process] $ExpectedProcess
    )
    if (-not ('WhykikiAudio.WindowsInstallerNativeMethods' -as [type])) {
        Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace WhykikiAudio
{
    public static class WindowsInstallerNativeMethods
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetNamedPipeClientProcessId(
            SafePipeHandle pipe, out uint clientProcessId);
    }
}
'@
    }
    [uint32] $clientProcessId = 0
    $identified = [WhykikiAudio.WindowsInstallerNativeMethods]::GetNamedPipeClientProcessId(
        $Pipe.SafePipeHandle, [ref]$clientProcessId)
    $nativeError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Assert-Condition $identified `
        "Cannot identify updater build-contract pipe client (Win32 error $nativeError)."
    Assert-Condition ($clientProcessId -eq [uint32]$ExpectedProcess.Id) `
        "Build-contract pipe client PID $clientProcessId is not the staged updater PID $($ExpectedProcess.Id)."
}

function Invoke-UpdaterBuildContract {
    param(
        [string] $Updater,
        [string] $Product,
        [string] $ProductVersion,
        [string] $Manufacturer,
        [string] $GitHubOwner,
        [string] $GitHubRepository,
        [string] $PayloadArchitecture,
        [string] $CurrentUpgradeCode,
        [string] $OtherUpgradeCode,
        [string] $SignerSha256
    )
    $challenge = New-CryptographicHex 32
    $pipeName = 'WhykikiAudio.UpdaterBuildContract.' + (New-CryptographicHex 16)
    $parentProcessId = [Convert]::ToString($PID, 10)
    $expectedResponse = '{"schema":"whykiki.windows-updater-build-contract",' +
        '"schemaVersion":1,"challenge":"' + $challenge + '",' +
        '"serverProcessId":' + $parentProcessId + ',' +
        '"buildMode":"production","compileOnly":false,' +
        '"product":"' + $Product + '","version":"' + $ProductVersion + '",' +
        '"manufacturer":"' + $Manufacturer + '",' +
        '"githubOwner":"' + $GitHubOwner + '",' +
        '"githubRepository":"' + $GitHubRepository + '",' +
        '"architecture":"' + $PayloadArchitecture + '",' +
        '"upgradeCode":"' + $CurrentUpgradeCode + '",' +
        '"otherUpgradeCode":"' + $OtherUpgradeCode + '",' +
        '"signerSha256":"' + $SignerSha256 + '"}' + "`n"
    [byte[]] $expectedBytes = [System.Text.UTF8Encoding]::new($false).GetBytes($expectedResponse)
    Assert-Condition ($expectedBytes.Length -le 4096) `
        'Expected updater build-contract response exceeds the fixed transport bound.'

    $pipeOptions = [System.IO.Pipes.PipeOptions](
        [int][System.IO.Pipes.PipeOptions]::Asynchronous -bor
        [int][System.IO.Pipes.PipeOptions]::CurrentUserOnly)
    $responsePipe = [System.IO.Pipes.NamedPipeServerStream]::new(
        $pipeName,
        [System.IO.Pipes.PipeDirection]::In,
        1,
        [System.IO.Pipes.PipeTransmissionMode]::Byte,
        $pipeOptions,
        4096,
        4096)
    $process = $null
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $connectionTask = $responsePipe.WaitForConnectionAsync()
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new($Updater)
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        foreach ($argument in @(
            '--validate-build-contract',
            '--challenge', $challenge,
            '--response-pipe', $pipeName,
            '--parent-process-id', $parentProcessId,
            '--product', $Product,
            '--version', $ProductVersion,
            '--manufacturer', $Manufacturer,
            '--github-owner', $GitHubOwner,
            '--github-repository', $GitHubRepository,
            '--architecture', $PayloadArchitecture,
            '--upgrade-code', $CurrentUpgradeCode,
            '--other-upgrade-code', $OtherUpgradeCode,
            '--signer-sha256', $SignerSha256
        )) {
            $startInfo.ArgumentList.Add($argument)
        }
        $process = [System.Diagnostics.Process]::Start($startInfo)
        Assert-Condition ($null -ne $process) 'The staged updater build contract could not be started.'

        while (-not $connectionTask.IsCompleted -and -not $process.HasExited) {
            $remaining = 30000 - [int]$stopwatch.ElapsedMilliseconds
            if ($remaining -le 0) {
                Stop-TimedOutProcess $process 'Updater build-contract validation' 30
            }
            [void]$connectionTask.Wait([Math]::Min(100, $remaining))
        }
        if (-not $connectionTask.IsCompleted) {
            $remaining = 30000 - [int]$stopwatch.ElapsedMilliseconds
            if ($remaining -gt 0) {
                [void]$connectionTask.Wait([Math]::Min(250, $remaining))
            }
        }
        Assert-Condition ($connectionTask.IsCompleted) `
            'Staged updater exited without connecting to its private build-contract response pipe.'
        [void]$connectionTask.GetAwaiter().GetResult()
        Assert-NamedPipeClientProcess $responsePipe $process

        # Read at most the exact canonical response plus one byte. The extra byte
        # makes BOMs, trailing whitespace, a second JSON value and all other
        # non-canonical responses fail without reading an unbounded stream.
        [byte[]] $received = [byte[]]::new($expectedBytes.Length + 1)
        $receivedCount = 0
        while ($receivedCount -lt $received.Length) {
            $readTask = $responsePipe.ReadAsync(
                $received, $receivedCount, $received.Length - $receivedCount)
            $remaining = 30000 - [int]$stopwatch.ElapsedMilliseconds
            if ($remaining -le 0 -or -not $readTask.Wait($remaining)) {
                Stop-TimedOutProcess $process 'Updater build-contract validation' 30
            }
            $read = [int]$readTask.GetAwaiter().GetResult()
            if ($read -eq 0) { break }
            $receivedCount += $read
        }

        $remaining = 30000 - [int]$stopwatch.ElapsedMilliseconds
        if ($remaining -le 0 -or -not $process.WaitForExit($remaining)) {
            Stop-TimedOutProcess $process 'Updater build-contract validation' 30
        }
        $exitCode = $process.ExitCode
        Assert-Condition ($receivedCount -eq $expectedBytes.Length) `
            "Staged updater returned a non-canonical build-contract response length: $receivedCount."
        $byteDifference = 0
        for ($index = 0; $index -lt $expectedBytes.Length; ++$index) {
            $byteDifference = $byteDifference -bor ($received[$index] -bxor $expectedBytes[$index])
        }
        Assert-Condition ($byteDifference -eq 0) `
            'Staged updater returned the wrong schema, challenge or compiled build identity.'
        Assert-Condition ($exitCode -eq 0) `
            "Staged updater rejected its exact production build contract with exit code $exitCode."
    }
    finally {
        $responsePipe.Dispose()
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) {
                    $process.Kill($true)
                    $process.WaitForExit()
                }
            }
            catch {}
            $process.Dispose()
        }
        $stopwatch.Stop()
    }
}

function Invoke-AdministrativeExtraction {
    param([string] $MsiPath, [string] $Destination, [string] $LogPath, [int] $TimeoutSeconds)
    [System.IO.Directory]::CreateDirectory($Destination) | Out-Null
    $msiexec = Join-Path $env:SystemRoot 'System32\msiexec.exe'
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new($msiexec)
    $startInfo.UseShellExecute = $false
    foreach ($argument in @('/a', $MsiPath, '/qn', "TARGETDIR=$Destination", '/L*v', $LogPath)) {
        $startInfo.ArgumentList.Add($argument)
    }
    $process = [System.Diagnostics.Process]::Start($startInfo)
    Assert-Condition ($null -ne $process) 'Windows Installer could not be started.'
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-TimedOutProcess $process 'Administrative MSI extraction' $TimeoutSeconds
    }
    $exitCode = $process.ExitCode
    $process.Dispose()
    if ($exitCode -ne 0) {
        $logTail = if (Test-Path -LiteralPath $LogPath -PathType Leaf) {
            (Get-Content -LiteralPath $LogPath -Tail 80 | Out-String).Trim()
        } else {
            '<Windows Installer did not create a log>'
        }
        throw "Administrative MSI extraction failed with exit code $exitCode.`n$logTail"
    }
}

$versionParts = @($Version.Split('.') | ForEach-Object { [int]$_ })
Assert-Condition ($versionParts[0] -le 255 -and $versionParts[1] -le 255 -and $versionParts[2] -le 65535) `
    'MSI versions require major/minor <= 255 and build <= 65535.'

$config = Get-Content -LiteralPath $script:PackageConfig -Raw | ConvertFrom-Json
Assert-Condition ($config.schemaVersion -eq 1) 'Unsupported installer config schema.'
$configPropertyNames = @($config.PSObject.Properties.Name | Sort-Object)
Assert-Condition (($configPropertyNames -join '|') -ceq
    'manufacturer|productName|schemaVersion|upgradeCodes|vst3Classes') `
    'package-config.json contains missing or unexpected top-level properties.'
$productName = [string]$config.productName
$manufacturer = [string]$config.manufacturer
Assert-Condition ($productName -match '^[A-Za-z0-9][A-Za-z0-9._ -]*$') 'Unsafe productName in package-config.json.'
Assert-Condition ($manufacturer -match '^[A-Za-z0-9][A-Za-z0-9._ -]{0,99}$') `
    'Manufacturer is missing or unsafe in package-config.json.'
$expectedVst3Classes = @($config.vst3Classes)
Assert-Condition ($expectedVst3Classes.Count -eq 2) `
    'package-config.json must declare exactly the processor and controller VST3 classes.'
foreach ($expectedClass in $expectedVst3Classes) {
    $classPropertyNames = @($expectedClass.PSObject.Properties.Name | Sort-Object)
    Assert-Condition (($classPropertyNames -join '|') -ceq 'category|cid') `
        'Each configured VST3 class must contain exactly cid and category.'
}
$expectedClassCategories = @($expectedVst3Classes.category | Sort-Object)
Assert-Condition (($expectedClassCategories -join '|') -ceq
    'Audio Module Class|Component Controller Class') `
    'package-config.json must declare the exact VST3 processor/controller categories.'
$githubOwner = 'TheWhykiki'
$githubRepository = $productName
$upgradeCode = Normalize-Guid ([string]$config.upgradeCodes.PSObject.Properties[$Architecture].Value)
$otherArchitecture = if ($Architecture -ceq 'x64') { 'arm64ec' } else { 'x64' }
$otherUpgradeCode = Normalize-Guid ([string]$config.upgradeCodes.PSObject.Properties[$otherArchitecture].Value)
Assert-Condition ($upgradeCode -cne $otherUpgradeCode) 'Architecture UpgradeCodes must be distinct.'
$productCode = New-NameBasedGuid ([guid]$upgradeCode) "$manufacturer/$productName/$Architecture/$Version/ProductCode"

$architectureContract = if ($Architecture -ceq 'x64') {
    [pscustomobject]@{ Vst3Directory = 'x86_64-win'; MsiArchitecture = 'x64'; Artifact = 'x64'; Display = 'Windows x64' }
} else {
    [pscustomobject]@{ Vst3Directory = 'arm64ec-win'; MsiArchitecture = 'arm64'; Artifact = 'arm64ec'; Display = 'Windows on Arm (ARM64EC)' }
}
$otherDisplay = if ($Architecture -ceq 'x64') { 'Windows on Arm (ARM64EC)' } else { 'Windows x64' }
$displayName = "$productName VST3 - $($architectureContract.Display)"

$bundleInput = Get-FullPath $BundlePath
Assert-Condition (Test-Path -LiteralPath $bundleInput -PathType Container) "VST3 bundle was not found: $bundleInput"
$bundleItem = Get-Item -LiteralPath $bundleInput -Force
Assert-Condition (($bundleItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -eq 0) `
    "VST3 bundle must not be a reparse point: $bundleInput"
Assert-Condition ($bundleItem.Name -ceq "$productName.vst3") `
    "Expected bundle name '$productName.vst3', found '$($bundleItem.Name)'."

$updaterRelativePaths = [System.Collections.Generic.List[string]]::new()
foreach ($updater in $UpdaterPath) {
    $updaterFile = Resolve-ExistingFile $updater 'Updater'
    $relativeUpdater = Get-RelativePathInside $bundleInput $updaterFile 'Updater'
    Assert-Condition ([System.IO.Path]::GetExtension($updaterFile) -in @('.exe', '.dll')) `
        "Updater must be an executable PE file: $updaterFile"
    $updaterRelativePaths.Add($relativeUpdater)
}
$updaterRelativePaths = @($updaterRelativePaths.ToArray() | Sort-Object -Unique)

if ($AllowUnsigned) {
    Assert-Condition (-not $CertificateThumbprint -and -not $CertificateSubject -and -not $TimestampUrl -and
                      -not $SignToolPath -and -not $UseMachineCertificateStore -and
                      -not $ExpectedSignerSha256) `
        'Do not pass signing options together with -AllowUnsigned.'
} else {
    Assert-Condition ($SourceCommit -cmatch '^[0-9a-f]{40}$') `
        'Production mode requires -SourceCommit as exactly 40 lowercase hexadecimal characters.'
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($HostTestPath)) `
        'Production mode requires -HostTestPath.'
    Assert-Condition (($CertificateThumbprint -xor $CertificateSubject)) `
        'Production mode requires exactly one of -CertificateThumbprint or -CertificateSubject.'
    Assert-Condition ($TimestampUrl) 'Production mode requires -TimestampUrl.'
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($ExpectedSignerSha256)) `
        'Production mode requires -ExpectedSignerSha256.'
    $ExpectedSignerSha256 = $ExpectedSignerSha256.Replace(' ', '').ToUpperInvariant()
    Assert-Condition ($ExpectedSignerSha256 -match '^[0-9A-F]{64}$') `
        'Production mode requires the exact 64-hex -ExpectedSignerSha256 compiled into the updater.'
    Assert-Condition ($UpdaterPath.Count -eq 1 -and $updaterRelativePaths.Count -eq 1 -and
                      $updaterRelativePaths[0] -ceq "Contents\Helpers\$($productName)Updater.exe") `
        'Production packages require exactly the product updater at Contents\Helpers\<Product>Updater.exe.'
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($CertificateStoreName)) `
        'CertificateStoreName must not be empty.'
    if ($CertificateSubject) {
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($CertificateSubject)) `
            'CertificateSubject must not be empty.'
    }
    $timestamp = [uri]$TimestampUrl
    Assert-Condition ($timestamp.IsAbsoluteUri -and $timestamp.Scheme -ceq 'https') `
        'TimestampUrl must be an absolute HTTPS RFC3161 endpoint.'
}

$outputRoot = Get-FullPath $OutputDirectory
$bundlePrefix = $bundleInput.TrimEnd([char[]]@('\', '/')) + [System.IO.Path]::DirectorySeparatorChar
Assert-Condition (-not ($outputRoot + [System.IO.Path]::DirectorySeparatorChar).StartsWith(
        $bundlePrefix, [System.StringComparison]::OrdinalIgnoreCase)) `
    'OutputDirectory must not be inside the VST3 payload.'
[System.IO.Directory]::CreateDirectory($outputRoot) | Out-Null
$distributionMarker = if ($AllowUnsigned) { '-UNSIGNED-NOT-FOR-DISTRIBUTION' } else { '' }
$artifactBase = "$productName-$Version-Windows-$($architectureContract.Artifact)$distributionMarker"
$finalCandidate = Join-Path $outputRoot $artifactBase
Assert-Condition (-not (Test-Path -LiteralPath $finalCandidate)) `
    "Refusing to overwrite an existing installer candidate: $finalCandidate"

$workRoot = Join-Path $outputRoot ('.installer-work-' + [guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($workRoot) | Out-Null
try {
    $policyMutationTestCount = Invoke-InstallerPolicyMutationTests $workRoot
    $initialSnapshot = Get-SafeTreeSnapshot $bundleInput
    Assert-Condition ($initialSnapshot.Files.Count -gt 0) 'The VST3 bundle is empty.'
    $sourceArchitectureDirectories = @($initialSnapshot.Directories | Where-Object { $_ -match '^Contents\\[^\\]+-win$' })
    Assert-Condition ($sourceArchitectureDirectories.Count -eq 1 -and
                      $sourceArchitectureDirectories[0] -ceq "Contents\$($architectureContract.Vst3Directory)") `
        "The VST3 bundle does not contain exactly the expected $($architectureContract.Vst3Directory) payload."
    $expectedBinaryRelative = "Contents\$($architectureContract.Vst3Directory)\$productName.vst3"
    $moduleInfoRelative = 'Contents\Resources\moduleinfo.json'
    Assert-Condition ($initialSnapshot.Files.RelativePath -ccontains $expectedBinaryRelative) `
        "Expected plug-in binary is missing: $expectedBinaryRelative"
    Assert-Condition ($initialSnapshot.Files.RelativePath -ccontains $moduleInfoRelative) `
        "VST3 moduleinfo.json is missing: $moduleInfoRelative"
    $moduleInfoRecord = @($initialSnapshot.Files | Where-Object { $_.RelativePath -ceq $moduleInfoRelative })
    Assert-Condition ($moduleInfoRecord.Count -eq 1 -and $moduleInfoRecord[0].Length -gt 0) `
        'VST3 moduleinfo.json is empty.'
    $moduleInfoContract = Test-ModuleInfoContract $moduleInfoRecord[0].FullName $productName `
        $manufacturer $Version $expectedVst3Classes
    $payloadContract = Get-PayloadExecutableContract $initialSnapshot $productName `
        $expectedBinaryRelative $AllowUnsigned.IsPresent
    $pluginBinaryRecord = @($initialSnapshot.Files | Where-Object {
        $_.RelativePath -ceq $expectedBinaryRelative
    })
    Assert-Condition ($pluginBinaryRecord.Count -eq 1) `
        'The primary VST3 PE identity is ambiguous in the payload snapshot.'
    $pluginVersionResourceContract = Test-PluginVersionResourceContract `
        $pluginBinaryRecord[0].FullName $productName $manufacturer $Version
    Assert-PolicyMutationRejected {
        Test-PluginVersionResourceContract $pluginBinaryRecord[0].FullName `
            $productName $manufacturer '255.255.65535'
    } 'FileVersion does not match' 'primary VST3 version resource'
    Assert-PolicyMutationRejected {
        Test-PluginVersionResourceContract $pluginBinaryRecord[0].FullName `
            $productName 'Wrong Manufacturer' $Version
    } 'CompanyName does not match' 'primary VST3 company resource'
    Assert-PolicyMutationRejected {
        Test-PluginVersionResourceContract $pluginBinaryRecord[0].FullName `
            'WrongProduct' $manufacturer $Version
    } 'ProductName does not match' 'primary VST3 product resource'
    $pluginVersionResourceMutationTestCount = 3
    foreach ($relativeUpdater in $updaterRelativePaths) {
        Assert-Condition ($payloadContract.PortableExecutablePaths -ccontains $relativeUpdater) `
            "UpdaterPath does not identify a structurally valid PE in the payload: $relativeUpdater"
    }
    $updaterVersionResourceContract = $null
    $updaterVersionResourceMutationTestCount = 0
    if (-not $AllowUnsigned) {
        $updaterBinaryRecord = @($initialSnapshot.Files | Where-Object {
            $_.RelativePath -ceq $updaterRelativePaths[0]
        })
        Assert-Condition ($updaterBinaryRecord.Count -eq 1) `
            'The production updater PE identity is ambiguous in the payload snapshot.'
        $updaterVersionResourceContract = Test-UpdaterVersionResourceContract `
            $updaterBinaryRecord[0].FullName $productName $manufacturer $Version
        Assert-PolicyMutationRejected {
            Test-UpdaterVersionResourceContract $updaterBinaryRecord[0].FullName `
                $productName $manufacturer '255.255.65535'
        } 'FileVersion does not match' 'updater version resource'
        Assert-PolicyMutationRejected {
            Test-UpdaterVersionResourceContract $updaterBinaryRecord[0].FullName `
                $productName 'Wrong Manufacturer' $Version
        } 'CompanyName does not match' 'updater company resource'
        Assert-PolicyMutationRejected {
            Test-UpdaterVersionResourceContract $updaterBinaryRecord[0].FullName `
                'WrongProduct' $manufacturer $Version
        } 'ProductName does not match' 'updater product resource'
        $updaterVersionResourceMutationTestCount = 3
    }

    $stagedBundle = Join-Path $workRoot "payload\$productName.vst3"
    Copy-Snapshot $initialSnapshot $stagedBundle
    $copiedSnapshot = Get-SafeTreeSnapshot $stagedBundle
    Assert-SameSnapshot $initialSnapshot $copiedSnapshot 'Staging copy'
    $sourceAfterCopy = Get-SafeTreeSnapshot $bundleInput
    Assert-SameSnapshot $initialSnapshot $sourceAfterCopy 'Source changed while staging'

    $dumpbin = Resolve-Dumpbin $DumpbinPath
    foreach ($relativePe in $payloadContract.PortableExecutablePaths) {
        Assert-PeArchitecture (Join-Path $stagedBundle $relativePe) $Architecture $dumpbin
    }
    if (-not $AllowUnsigned) {
        Invoke-UpdaterBuildContract (Join-Path $stagedBundle $updaterRelativePaths[0]) `
            $productName $Version $manufacturer $githubOwner $githubRepository $Architecture `
            $upgradeCode $otherUpgradeCode $ExpectedSignerSha256
    }

    [string[]] $signableRelativePaths = @($payloadContract.PortableExecutablePaths)

    $signTool = $null
    $signingCertificate = $null
    $signerCertificateSha256 = $null
    if (-not $AllowUnsigned) {
        $signTool = Resolve-SignTool $SignToolPath
        $signingCertificate = Resolve-SigningCertificate $CertificateThumbprint $CertificateSubject `
            $CertificateStoreName $UseMachineCertificateStore.IsPresent
        $signerCertificateSha256 = $signingCertificate.Sha256
        Assert-Condition ($signerCertificateSha256 -ceq $ExpectedSignerSha256) `
            'Selected signing certificate does not match the SHA-256 fingerprint compiled into the updater.'
        foreach ($relative in $signableRelativePaths) {
            $signResult = Invoke-AuthenticodeSign (Join-Path $stagedBundle $relative) $signTool `
                $signingCertificate.Arguments $signingCertificate.Thumbprint $timestamp
            Assert-Condition ($signResult.SignerSha256 -ceq $signerCertificateSha256) `
                "Signer SHA-256 fingerprint mismatch after signing: $relative"
        }
    }

    $signedSnapshot = Get-SafeTreeSnapshot $stagedBundle
    Assert-Condition ($signedSnapshot.Files.Count -eq $initialSnapshot.Files.Count) `
        'Signing changed the set of payload files.'
    Assert-Condition ($signedSnapshot.Directories.Count -eq $initialSnapshot.Directories.Count) `
        'Signing changed the set of payload directories.'
    for ($index = 0; $index -lt $initialSnapshot.Directories.Count; ++$index) {
        Assert-Condition ($initialSnapshot.Directories[$index] -ceq $signedSnapshot.Directories[$index]) `
            'Signing changed a payload directory.'
    }
    for ($index = 0; $index -lt $initialSnapshot.Files.Count; ++$index) {
        $before = $initialSnapshot.Files[$index]
        $after = $signedSnapshot.Files[$index]
        Assert-Condition ($before.RelativePath -ceq $after.RelativePath) 'Signing changed a payload path.'
        if ($signableRelativePaths -cnotcontains $before.RelativePath) {
            Assert-Condition ($before.Length -eq $after.Length -and $before.Sha256 -ceq $after.Sha256) `
                "Signing changed a non-PE payload file: $($before.RelativePath)"
        }
    }
    $signedPayloadContract = Get-PayloadExecutableContract $signedSnapshot $productName `
        $expectedBinaryRelative $AllowUnsigned.IsPresent
    Assert-Condition (($signedPayloadContract.PortableExecutablePaths -cjoin "`n") -ceq
                      ($payloadContract.PortableExecutablePaths -cjoin "`n")) `
        'Signing changed the classified PE payload set.'
    foreach ($relativePe in $signedPayloadContract.PortableExecutablePaths) {
        Assert-PeArchitecture (Join-Path $stagedBundle $relativePe) $Architecture $dumpbin
    }
    if (-not $AllowUnsigned) {
        $updaterVersionResourceContract = Test-UpdaterVersionResourceContract `
            (Join-Path $stagedBundle $updaterRelativePaths[0]) `
            $productName $manufacturer $Version
    }

    Restore-PinnedWix
    $msiPath = Join-Path $workRoot "$artifactBase.msi"
    $wixPdb = Join-Path $workRoot "$artifactBase.wixpdb"
    $wixArguments = @(
        'build', $script:PackageSource,
        '-arch', $architectureContract.MsiArchitecture,
        '-bindpath', "Payload=$stagedBundle",
        '-d', "ProductName=$productName",
        '-d', "DisplayName=$displayName",
        '-d', "Manufacturer=$manufacturer",
        '-d', "ProductVersion=$Version",
        '-d', "ProductCode=$productCode",
        '-d', "UpgradeCode=$upgradeCode",
        '-d', "OtherArchitectureUpgradeCode=$otherUpgradeCode",
        '-d', "OtherArchitectureName=$otherDisplay",
        '-intermediateFolder', (Join-Path $workRoot 'wix-obj'),
        '-pdb', $wixPdb,
        '-out', $msiPath
    )
    Invoke-Wix $wixArguments | Out-Null
    Assert-Condition (Test-Path -LiteralPath $msiPath -PathType Leaf) 'WiX did not produce the MSI.'
    $stagingAfterBuild = Get-SafeTreeSnapshot $stagedBundle
    Assert-SameSnapshot $signedSnapshot $stagingAfterBuild 'Packaging source changed during WiX build'
    Invoke-Wix @('msi', 'validate', $msiPath, '-pdb', $wixPdb) | Out-Null

    $msiContract = Test-MsiContract $msiPath $productName $displayName $manufacturer $Version `
        $productCode $upgradeCode `
        $otherUpgradeCode $architectureContract.MsiArchitecture $signedSnapshot.Files.Count

    if (-not $AllowUnsigned) {
        $msiSignResult = Invoke-AuthenticodeSign $msiPath $signTool $signingCertificate.Arguments `
            $signingCertificate.Thumbprint $timestamp
        Assert-Condition ($msiSignResult.SignerSha256 -ceq $signerCertificateSha256) `
            'MSI signer SHA-256 fingerprint differs from the payload signer.'
        $postSignContract = Test-MsiContract $msiPath $productName $displayName $manufacturer $Version `
            $productCode $upgradeCode `
            $otherUpgradeCode $architectureContract.MsiArchitecture $signedSnapshot.Files.Count
        Assert-Condition ($postSignContract.Template -ceq $msiContract.Template -and
                          $postSignContract.FileRows -eq $msiContract.FileRows) `
            'MSI tables changed unexpectedly during signing.'
    }

    $administrativeRoot = Join-Path $workRoot 'administrative-image'
    $administrativeLog = Join-Path $workRoot 'administrative-install.log'
    Invoke-AdministrativeExtraction $msiPath $administrativeRoot $administrativeLog `
        $AdministrativeExtractionTimeoutSeconds
    $administrativeSnapshot = Get-SafeTreeSnapshot $administrativeRoot
    $bundleDirectories = @($administrativeSnapshot.Directories | Where-Object {
        [System.IO.Path]::GetExtension($_) -ieq '.vst3'
    })
    Assert-Condition ($bundleDirectories.Count -eq 1) `
        "Administrative extraction produced $($bundleDirectories.Count) VST3 bundle directories."
    $bundleRelativePath = $bundleDirectories[0]
    Assert-Condition ([System.IO.Path]::GetFileName($bundleRelativePath) -ceq "$productName.vst3") `
        "Administrative extraction produced the wrong VST3 bundle: $bundleRelativePath"
    $bundleRelativePrefix = $bundleRelativePath.TrimEnd('\') + '\'
    $allowedAncestorDirectories = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $ancestor = [System.IO.Path]::GetDirectoryName($bundleRelativePath)
    while ($ancestor) {
        [void]$allowedAncestorDirectories.Add($ancestor)
        $ancestor = [System.IO.Path]::GetDirectoryName($ancestor)
    }
    foreach ($relativeDirectory in $administrativeSnapshot.Directories) {
        $insideBundle = $relativeDirectory -ieq $bundleRelativePath -or
            $relativeDirectory.StartsWith($bundleRelativePrefix,
                [System.StringComparison]::OrdinalIgnoreCase)
        Assert-Condition ($insideBundle -or $allowedAncestorDirectories.Contains($relativeDirectory)) `
            "Administrative image contains a directory outside the single VST3 payload: $relativeDirectory"
    }
    $administrativeMsiFiles = 0
    $expectedAdministrativeMsiName = [System.IO.Path]::GetFileName($msiPath)
    foreach ($file in $administrativeSnapshot.Files) {
        if ($file.RelativePath.StartsWith($bundleRelativePrefix,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $isExpectedAdministrativeMsi = [string]::IsNullOrEmpty(
            [System.IO.Path]::GetDirectoryName($file.RelativePath)) -and
            [System.IO.Path]::GetFileName($file.RelativePath) -ieq $expectedAdministrativeMsiName
        if ($isExpectedAdministrativeMsi) { ++$administrativeMsiFiles }
        Assert-Condition ($isExpectedAdministrativeMsi -and $administrativeMsiFiles -eq 1) `
            "Administrative image contains a file outside the single VST3 payload: $($file.RelativePath)"
    }
    $extractedBundle = Join-Path $administrativeRoot $bundleRelativePath
    $extractedSnapshot = Get-SafeTreeSnapshot $extractedBundle
    Assert-SameSnapshot $signedSnapshot $extractedSnapshot 'Administrative extraction'

    $hostTestRan = $false
    if ($HostTestPath) {
        $hostTest = Resolve-ExistingFile $HostTestPath 'VST3 host test'
        Assert-Condition ([System.IO.Path]::GetExtension($hostTest) -ieq '.exe') `
            'VST3 host test must be a Windows executable.'
        $hostStartInfo = [System.Diagnostics.ProcessStartInfo]::new($hostTest)
        $hostStartInfo.UseShellExecute = $false
        $hostStartInfo.ArgumentList.Add($extractedBundle)
        $hostProcess = [System.Diagnostics.Process]::Start($hostStartInfo)
        Assert-Condition ($null -ne $hostProcess) 'VST3 host test could not be started.'
        if (-not $hostProcess.WaitForExit($HostTestTimeoutSeconds * 1000)) {
            Stop-TimedOutProcess $hostProcess 'VST3 host test' $HostTestTimeoutSeconds
        }
        $hostExitCode = $hostProcess.ExitCode
        $hostProcess.Dispose()
        Assert-Condition ($hostExitCode -eq 0) `
            "VST3 host test rejected the extracted bundle with exit code $hostExitCode."
        $hostTestRan = $true
    }

    $candidate = Join-Path $workRoot 'candidate'
    [System.IO.Directory]::CreateDirectory($candidate) | Out-Null
    $candidateMsi = Join-Path $candidate "$artifactBase.msi"
    [System.IO.File]::Move($msiPath, $candidateMsi)
    $payloadEvidence = @($signedSnapshot.Files | ForEach-Object {
        [ordered]@{ path = $_.RelativePath; size = $_.Length; sha256 = $_.Sha256 }
    })
    $timestampUrlHash = $null
    if (-not $AllowUnsigned) {
        $timestampBytes = [System.Text.Encoding]::UTF8.GetBytes($timestamp.AbsoluteUri)
        $timestampUrlHash = [Convert]::ToHexString(
            [System.Security.Cryptography.SHA256]::HashData($timestampBytes))
    }
    $evidence = [ordered]@{
        schemaVersion = 2
        artifactStatus = if ($AllowUnsigned) { 'UNSIGNED-NOT-FOR-DISTRIBUTION' } else { 'SIGNED' }
        product = $productName
        version = $Version
        sourceCommit = if ($AllowUnsigned) { $null } else { $SourceCommit }
        payloadArchitecture = $Architecture
        msiArchitecture = $architectureContract.MsiArchitecture
        wixVersion = $script:RequiredWixVersion
        wixToolManifestSha256 = (Get-FileHash -LiteralPath $script:ToolManifest -Algorithm SHA256).Hash.ToUpperInvariant()
        nuGetConfigSha256 = (Get-FileHash -LiteralPath $script:NuGetConfig -Algorithm SHA256).Hash.ToUpperInvariant()
        productCode = $productCode
        upgradeCode = $upgradeCode
        otherArchitectureUpgradeCode = $otherUpgradeCode
        msiFile = [System.IO.Path]::GetFileName($candidateMsi)
        msiSha256 = (Get-FileHash -LiteralPath $candidateMsi -Algorithm SHA256).Hash.ToUpperInvariant()
        signed = -not $AllowUnsigned
        signerCertificateSha256 = $signerCertificateSha256
        updaterSignerPinSha256 = if ($AllowUnsigned) { $null } else { $ExpectedSignerSha256 }
        signingDigest = if ($AllowUnsigned) { $null } else { 'SHA256' }
        timestampProtocol = if ($AllowUnsigned) { $null } else { 'RFC3161-SHA256' }
        timestampUrlSha256 = $timestampUrlHash
        updaterPaths = @($updaterRelativePaths)
        moduleInfo = [ordered]@{
            path = $moduleInfoRelative
            sha256 = $moduleInfoContract.Sha256
            name = $productName
            manufacturer = $manufacturer
            version = $Version
            classIdentities = @($moduleInfoContract.ClassIdentities)
        }
        pluginVersionResource = [ordered]@{
            fileVersion = $pluginVersionResourceContract.FileVersion
            productVersion = $pluginVersionResourceContract.ProductVersion
            companyName = $pluginVersionResourceContract.CompanyName
            productName = $pluginVersionResourceContract.ProductName
            fileDescription = $pluginVersionResourceContract.FileDescription
            mutationTests = $pluginVersionResourceMutationTestCount
        }
        updaterVersionResource = if ($AllowUnsigned) { $null } else { [ordered]@{
            fileVersion = $updaterVersionResourceContract.FileVersion
            productVersion = $updaterVersionResourceContract.ProductVersion
            companyName = $updaterVersionResourceContract.CompanyName
            productName = $updaterVersionResourceContract.ProductName
            fileDescription = $updaterVersionResourceContract.FileDescription
            internalName = $updaterVersionResourceContract.InternalName
            originalFilename = $updaterVersionResourceContract.OriginalFilename
            mutationTests = $updaterVersionResourceMutationTestCount
        } }
        payloadClassification = [ordered]@{
            portableExecutablePaths = @($signedPayloadContract.PortableExecutablePaths)
            helperPortableExecutablePaths = @($signedPayloadContract.HelperPortableExecutablePaths)
            updaterLikePortableExecutablePaths = @($signedPayloadContract.UpdaterLikePortableExecutablePaths)
            forbiddenExecutableExtensions = 0
            malformedExecutableExtensions = 0
            everyPortableExecutableArchitectureValidated = $true
            signedPortableExecutablePaths = if ($AllowUnsigned) { @() } else { @($signableRelativePaths) }
        }
        payloadFiles = $payloadEvidence
        validation = [ordered]@{
            policyMutationTests = $policyMutationTestCount
            moduleInfoIdentityValidated = $true
            pluginVersionResourceValidated = $true
            updaterVersionResourceValidated = -not $AllowUnsigned
            payloadClassificationValidated = $true
            wixIce = $true
            customActions = 0
            forbiddenSideEffectTables = 0
            directoryGraphValidated = $true
            componentContainmentValidated = $true
            fileComponentReferencesValidated = $true
            forbiddenSequenceActions = 0
            packageTemplate = $msiContract.Template
            fileTableRows = $msiContract.FileRows
            componentTableRows = $msiContract.ComponentRows
            displayName = $msiContract.ProductName
            manufacturer = $msiContract.Manufacturer
            productLanguage = $msiContract.ProductLanguage
            msiDeploymentCompliant = $msiContract.DeploymentCompliant
            secureCustomProperties = @($msiContract.SecureCustomProperties)
            launchConditions = @($msiContract.LaunchConditions)
            removeExistingProductsSequence = $msiContract.RemoveExistingProductsSequence
            administrativeExtractionHashMatch = $true
            administrativeImageLayoutValidated = $true
            hostTestRan = $hostTestRan
            administrativeExtractionTimeoutSeconds = $AdministrativeExtractionTimeoutSeconds
            hostTestTimeoutSeconds = $HostTestTimeoutSeconds
        }
    }
    $evidencePath = Join-Path $candidate "$artifactBase.evidence.json"
    $evidenceJson = $evidence | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($evidencePath, $evidenceJson + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.Directory]::Move($candidate, $finalCandidate)
    Write-Host "Windows installer candidate: $finalCandidate"
    Write-Host "Evidence: $(Join-Path $finalCandidate ([System.IO.Path]::GetFileName($evidencePath)))"
}
finally {
    if (Test-Path -LiteralPath $workRoot -PathType Container) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    }
}
