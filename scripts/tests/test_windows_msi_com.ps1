# SPDX-License-Identifier: AGPL-3.0-only
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Import only the production helpers: never run the packager, download tools,
# install a package or depend on an already-built plugin in this smoke test.
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot '../build-windows-installer.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Packager PowerShell parse failed.' }
foreach ($name in @('Assert-Condition', 'Invoke-ComMember', 'Invoke-ComMethod',
                    'Get-ComProperty', 'Set-ComProperty', 'Get-MsiRows',
                    'Get-MsiSummaryProperty', 'Invoke-MsiSequenceMutationTests',
                    'Invoke-MsiArchitectureMutationTest')) {
    $definition = @($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $name
    }, $false))
    if ($definition.Count -ne 1) { throw "Missing or duplicate helper: $name" }
    . ([scriptblock]::Create($definition[0].Extent.Text))
}

# This reproduces the pre-fix failure even without Windows: Join-Path's output
# looks like String in PowerShell, but reflection sees the PSObject wrapper.
$wrappedPath = Join-Path ([System.IO.Path]::GetTempPath()) 'msi-reflection-fixture.msi'
Assert-Condition ($wrappedPath -is [psobject]) 'Regression did not create a wrapped path.'
$rawRejected = $false
try {
    [void]$wrappedPath.GetType().InvokeMember('StartsWith',
        [System.Reflection.BindingFlags]::InvokeMethod, $null, $wrappedPath, @($wrappedPath))
} catch { $rawRejected = $true }
Assert-Condition $rawRejected 'Unnormalized reflection unexpectedly accepted a PSObject string.'
Assert-Condition (Invoke-ComMethod $wrappedPath 'StartsWith' @($wrappedPath)) `
    'Wrapped string did not survive argument normalization.'

$builder = [System.Text.StringBuilder]::new('test')
$wrappedInteger = [psobject]::AsPSObject(256)
Set-ComProperty $builder 'Capacity' @($wrappedInteger)
Assert-Condition ((Get-ComProperty $builder 'Capacity') -eq 256) 'Integer setter changed type/value.'
Assert-Condition ((Get-ComProperty $builder 'Chars' @([psobject]::AsPSObject(0))) -ceq 't') `
    'Indexed getter did not normalize its integer argument.'
[void](Invoke-ComMethod $builder 'Clear')
Assert-Condition ((Get-ComProperty $builder 'Length') -eq 0) 'Empty argument list changed.'
[void](Invoke-ComMethod $builder 'Append' @($wrappedPath))
$items = [System.Collections.ArrayList]::new()
[void](Invoke-ComMethod $items 'Add' @($null))
Assert-Condition ($items.Count -eq 1 -and $null -eq $items[0]) 'Null argument was not preserved.'
$diagnostic = ''
try { Invoke-ComMethod $builder 'MethodDoesNotExist' @($wrappedPath, $wrappedInteger, $null) }
catch { $diagnostic = $_.Exception.Message }
Assert-Condition ($diagnostic -like "MSI COM InvokeMethod 'MethodDoesNotExist' (System.String, System.Int32, null) failed:*") `
    'COM failure did not identify the member and normalized argument types.'
Write-Host 'PASS: MSI reflection argument transport (wrapped string/int, null, zero args, get/set, diagnostics).'

if (-not $IsWindows) {
    Write-Host 'SKIP: actual Windows Installer COM smoke test requires Windows.'
    exit 0
}

function Invoke-FixtureSql([object] $Database, [string] $Query) {
    $view = Invoke-ComMethod $Database 'OpenView' @($Query)
    try {
        [void](Invoke-ComMethod $view 'Execute')
        [void](Invoke-ComMethod $view 'Close')
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view) }
}

function Assert-FixtureSequence([string] $Path) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $null
    try {
        $database = Invoke-ComMethod $installer 'OpenDatabase' @($Path, 0)
        $rows = @(Get-MsiRows $database 'SELECT `Action`, `Condition`, `Sequence` FROM `InstallExecuteSequence`')
        $sequences = @{}
        foreach ($row in $rows) {
            $sequence = 0
            Assert-Condition ([string]::IsNullOrEmpty($row.Fields[1]) -and
                [int]::TryParse($row.Fields[2], [ref]$sequence) -and $sequence -gt 0) `
                'MSI sequence action must be unconditional and positive.'
            $sequences.Add($row.Fields[0], $sequence)
        }
        $previous = 0
        foreach ($action in @('FindRelatedProducts', 'LaunchConditions', 'InstallInitialize',
                              'RemoveExistingProducts', 'InstallFiles')) {
            Assert-Condition ($sequences.ContainsKey($action) -and $sequences[$action] -gt $previous) `
                'MSI detection/launch-condition/major-upgrade sequence is unsafe.'
            $previous = $sequences[$action]
        }
        Assert-Condition ($sequences['MigrateFeatureStates'] -gt $sequences['FindRelatedProducts']) `
            'FindRelatedProducts must run before MigrateFeatureStates.'
    } finally {
        if ($null -ne $database) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
    }
}

function Assert-FixtureArchitecture([string] $Path) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    try {
        Assert-Condition ((Get-MsiSummaryProperty $installer $Path 7) -ceq 'x64;1033') `
            'Expected x64 MSI template.'
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) }
}

$fixtureRoot = [System.IO.Directory]::CreateTempSubdirectory('whykiki-msi-com-').FullName
try {
    $path = Join-Path $fixtureRoot 'fixture.msi'
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $null
    try {
        $database = Invoke-ComMethod $installer 'OpenDatabase' @($path, 3)
        Invoke-FixtureSql $database 'CREATE TABLE `InstallExecuteSequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)'
        foreach ($entry in @(@('FindRelatedProducts', 25), @('LaunchConditions', 100),
                             @('MigrateFeatureStates', 1200), @('InstallInitialize', 1500),
                             @('RemoveExistingProducts', 1501), @('InstallFiles', 4000))) {
            Invoke-FixtureSql $database ("INSERT INTO ``InstallExecuteSequence`` (``Action``, ``Sequence``) VALUES ('{0}', {1})" -f $entry[0], $entry[1])
        }
        [void](Invoke-ComMethod $database 'Commit')
    } finally {
        if ($null -ne $database) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
    }
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $summary = $null
    try {
        $summary = Get-ComProperty $installer 'SummaryInformation' @($path, 1)
        Set-ComProperty $summary 'Property' @(7, 'x64;1033')
        [void](Invoke-ComMethod $summary 'Persist')
    } finally {
        if ($null -ne $summary) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($summary) }
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer)
    }
    Assert-FixtureSequence $path
    Assert-FixtureArchitecture $path
    $count = Invoke-MsiSequenceMutationTests $path $fixtureRoot { param($candidate) Assert-FixtureSequence $candidate } '' '1.0.0'
    Assert-Condition ($count -eq 14) 'Wrong sequence mutation count.'
    $count = Invoke-MsiArchitectureMutationTest $path $fixtureRoot { param($candidate) Assert-FixtureArchitecture $candidate } '' '1.0.0'
    Assert-Condition ($count -eq 1) 'Wrong architecture mutation count.'
    # Infrastructure/COM failures must not be counted as policy rejections.
    $unexpected = ''
    try {
        Invoke-MsiSequenceMutationTests $path $fixtureRoot { throw 'injected transport failure' } '' '1.0.0'
    } catch { $unexpected = $_.Exception.Message }
    Assert-Condition ($unexpected -ceq 'injected transport failure') 'Transport failure was swallowed as a rejected mutant.'
    Assert-FixtureSequence $path
    Assert-FixtureArchitecture $path
    Write-Host 'PASS: Windows Installer COM database/summary baseline, 14 sequence + 1 architecture mutants, transport failures remain fatal.'
} finally {
    # The only recursive target is this test-created private directory.
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
}
