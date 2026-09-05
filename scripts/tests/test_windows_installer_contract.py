#!/usr/bin/env python3
"""Static, cross-platform contracts for the Windows MSI release layer."""

from __future__ import annotations

import json
import re
import unittest
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WIX_NAMESPACE = "http://wixtoolset.org/schemas/v4/wxs"
UPGRADE_NAMESPACE = uuid.UUID("bd4ae2ea-c1c6-5d51-a960-55f67866e15b")


class WindowsInstallerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = (ROOT / "scripts" / "build-windows-installer.ps1").read_text(
            encoding="utf-8"
        )
        cls.wxs_text = (ROOT / "Installer" / "Windows" / "Package.wxs").read_text(
            encoding="utf-8"
        )
        cls.wxs = ET.fromstring(cls.wxs_text)
        cls.config = json.loads(
            (ROOT / "Installer" / "Windows" / "package-config.json").read_text(
                encoding="utf-8"
            )
        )
        cls.tool_manifest = json.loads(
            (ROOT / ".config" / "dotnet-tools.json").read_text(encoding="utf-8")
        )
        cls.nuget = (ROOT / "Installer" / "Windows" / "NuGet.Config").read_text(
            encoding="utf-8"
        )
        cls.docs = (ROOT / "WINDOWS_INSTALLER.md").read_text(encoding="utf-8")

    def elements(self, name: str) -> list[ET.Element]:
        return list(self.wxs.iter(f"{{{WIX_NAMESPACE}}}{name}"))

    def test_wix_is_exactly_pinned_and_signature_trusted(self) -> None:
        self.assertEqual(
            self.tool_manifest,
            {
                "version": 1,
                "isRoot": True,
                "tools": {"wix": {"version": "6.0.2", "commands": ["wix"]}},
            },
        )
        self.assertEqual(self.wxs.attrib["RequiredVersion"], "6.0.2")
        self.assertIn('signatureValidationMode" value="require"', self.nuget)
        self.assertIn(
            "D95336DD2022934D80E3F3A4F938DD66EC7076BBBA680F76C11F2B54B346D61D",
            self.nuget,
        )
        for token in (
            "--tool-manifest $script:ToolManifest",
            "--configfile $script:NuGetConfig",
            "--disable-parallel",
            "--no-cache",
            "RequiredNuGetSignerFingerprint",
            "https://api.nuget.org/v3/index.json",
            "SelectNodes('/configuration/trustedSigners/author/certificate')",
            "SelectNodes('/configuration/packageSources/clear')",
            "Restored WiX reported",
        ):
            self.assertIn(token, self.script)

    def test_product_configuration_has_stable_architecture_identities(self) -> None:
        self.assertEqual(self.config["schemaVersion"], 1)
        self.assertEqual(self.config["productName"], ROOT.name)
        self.assertEqual(self.config["manufacturer"], "Whykiki Audio")
        codes = self.config["upgradeCodes"]
        self.assertEqual(set(codes), {"x64", "arm64ec"})
        self.assertNotEqual(codes["x64"].casefold(), codes["arm64ec"].casefold())
        for architecture, value in codes.items():
            parsed = uuid.UUID(value)
            expected = uuid.uuid5(
                UPGRADE_NAMESPACE,
                f"Whykiki Audio/{ROOT.name}/Windows MSI/{architecture}/upgrade",
            )
            self.assertEqual(parsed, expected)
        expected_classes = {
            "SubLab808": {
                ("ABCDEF019182FAEB576B696B536C3038", "Audio Module Class"),
                ("ABCDEF011234ABCD576B696B536C3038", "Component Controller Class"),
            },
            "ReverseLab": {
                ("ABCDEF019182FAEB576B696B52764C62", "Audio Module Class"),
                ("ABCDEF011234ABCD576B696B52764C62", "Component Controller Class"),
            },
        }
        self.assertEqual(
            {(item["cid"], item["category"]) for item in self.config["vst3Classes"]},
            expected_classes[ROOT.name],
        )

    def test_package_is_per_machine_major_upgrade_without_custom_actions(self) -> None:
        package = self.elements("Package")
        self.assertEqual(len(package), 1)
        self.assertEqual(package[0].attrib["Scope"], "perMachine")
        self.assertEqual(package[0].attrib["InstallerVersion"], "500")
        self.assertEqual(package[0].attrib["ProductCode"], "$(ProductCode)")
        self.assertEqual(package[0].attrib["UpgradeCode"], "$(UpgradeCode)")
        deployment = self.elements("Property")
        self.assertEqual(len(deployment), 1)
        self.assertEqual(deployment[0].attrib, {"Id": "MSIDEPLOYMENTCOMPLIANT", "Value": "1"})
        upgrade = self.elements("MajorUpgrade")
        self.assertEqual(len(upgrade), 1)
        self.assertEqual(upgrade[0].attrib["AllowDowngrades"], "no")
        self.assertEqual(upgrade[0].attrib["AllowSameVersionUpgrades"], "no")
        self.assertEqual(upgrade[0].attrib["MigrateFeatures"], "yes")
        self.assertEqual(upgrade[0].attrib["IgnoreLanguage"], "no")
        self.assertEqual(upgrade[0].attrib["IgnoreRemoveFailure"], "no")
        self.assertEqual(upgrade[0].attrib["Schedule"], "afterInstallInitialize")
        self.assertIn("DowngradeErrorMessage", upgrade[0].attrib)
        self.assertFalse(self.elements("CustomAction"))
        self.assertFalse(self.elements("CustomActionRef"))
        self.assertFalse(self.elements("Binary"))
        self.assertIn("Forbidden MSI side-effect table is present", self.script)

    def test_generated_msi_repeats_runtime_safety_contract(self) -> None:
        graph = self.script[
            self.script.index("function Assert-MsiDirectoryContract") :
            self.script.index("function Test-MsiContract")
        ]
        for token in (
            "MSI Directory graph has no TARGETDIR root",
            "MSI TARGETDIR must be the only root",
            "foreach ($identifier in @($Directories.Keys))",
            "MSI Directory graph contains a cycle",
            "MSI Directory graph contains a dangling parent",
            "does not descend from TARGETDIR",
            "does not descend from INSTALLFOLDER",
            "MSI component directory refers to unknown Directory",
        ):
            self.assertIn(token, graph)

        directory_call = self.script.index("Assert-MsiDirectoryContract $directories")
        file_query = self.script.index("SELECT `File`, `Component_` FROM `File`")
        self.assertLess(directory_call, file_query)
        self.assertIn("$componentIds.Contains($row.Fields[1])", self.script)
        self.assertIn(
            "MSI File row refers to an unknown payload component", self.script
        )

        forbidden_match = re.search(
            r"\$forbiddenSideEffectTables\s*=\s*@\((.*?)\n\s*\)",
            self.script,
            re.DOTALL,
        )
        self.assertIsNotNone(forbidden_match)
        forbidden_tables = set(re.findall(r"'([^']+)'", forbidden_match.group(1)))
        self.assertEqual(
            forbidden_tables,
            {
                "CustomAction", "Binary", "ServiceInstall", "ServiceControl",
                "Registry", "RemoveRegistry", "SelfReg", "TypeLib", "Class",
                "ProgId", "Extension", "MIME", "AppId", "ODBCDataSource",
                "ODBCDriver", "ODBCTranslator", "IniFile", "RemoveIniFile",
                "Environment", "RemoveFile", "MoveFiles", "DuplicateFile",
                "CreateFolder", "Shortcut", "ReserveCost", "BindImage", "Font",
                "IsolatedComponent", "MsiAssembly", "MsiAssemblyName",
                "PublishComponent", "Complus", "Verb", "ODBCAttribute",
                "LockPermissions", "MsiLockPermissionsEx", "Permission",
                "PermissionEx", "Patch", "PatchPackage", "SFPCatalog",
            },
        )

        action_match = re.search(
            r"\$forbiddenSequenceActions\s*=\s*@\((.*?)\)", self.script, re.DOTALL
        )
        self.assertIsNotNone(action_match)
        self.assertEqual(
            set(re.findall(r"'([^']+)'", action_match.group(1))),
            {"ForceReboot", "ScheduleReboot", "DisableRollback"},
        )
        sequence_match = re.search(
            r"foreach \(\$sequenceTable in @\((.*?)\)\) \{",
            self.script,
            re.DOTALL,
        )
        self.assertIsNotNone(sequence_match)
        self.assertEqual(
            set(re.findall(r"'([^']+)'", sequence_match.group(1))),
            {
                "InstallExecuteSequence", "InstallUISequence",
                "AdminExecuteSequence", "AdminUISequence", "AdvtExecuteSequence",
            },
        )

    def test_administrative_image_has_no_unaccounted_entries(self) -> None:
        layout_start = self.script.index("$administrativeSnapshot = Get-SafeTreeSnapshot")
        hash_check = self.script.index(
            "Assert-SameSnapshot $signedSnapshot $extractedSnapshot",
            layout_start,
        )
        layout = self.script[layout_start:hash_check]
        for token in (
            "$bundleDirectories.Count -eq 1",
            'GetFileName($bundleRelativePath) -ceq "$productName.vst3"',
            "$allowedAncestorDirectories",
            "directory outside the single VST3 payload",
            "$expectedAdministrativeMsiName = [System.IO.Path]::GetFileName($msiPath)",
            "GetDirectoryName($file.RelativePath)",
            "$administrativeMsiFiles -eq 1",
            "file outside the single VST3 payload",
        ):
            self.assertIn(token, layout)
        for token in (
            "forbiddenSideEffectTables = 0",
            "updaterVersionResourceValidated = -not $AllowUnsigned",
            "directoryGraphValidated = $true",
            "componentContainmentValidated = $true",
            "fileComponentReferencesValidated = $true",
            "forbiddenSequenceActions = 0",
            "administrativeImageLayoutValidated = $true",
        ):
            self.assertIn(token, self.script)

    def test_target_and_complete_harvest_are_fixed(self) -> None:
        standard = self.elements("StandardDirectory")
        self.assertEqual(len(standard), 1)
        self.assertEqual(standard[0].attrib["Id"], "CommonFiles6432Folder")
        directories = {item.attrib["Id"]: item for item in self.elements("Directory")}
        self.assertEqual(directories["VST3Folder"].attrib["Name"], "VST3")
        self.assertEqual(directories["INSTALLFOLDER"].attrib["Name"], "$(ProductName).vst3")
        files = self.elements("Files")
        self.assertEqual(len(files), 1)
        self.assertEqual(files[0].attrib["Include"], r"!(bindpath.Payload)\**")
        self.assertEqual(files[0].attrib["Directory"], "INSTALLFOLDER")
        for token in (
            "Reparse points, symlinks and junctions are forbidden",
            "Alternate data streams are forbidden",
            "Source changed while staging",
            "Case-colliding payload path",
            "Packaging source changed during WiX build",
            "MSI File table has",
            "Administrative extraction",
            "Assert-SameSnapshot $signedSnapshot $extractedSnapshot",
        ):
            self.assertIn(token, self.script)

    def test_architecture_packages_are_mutually_exclusive(self) -> None:
        upgrades = self.elements("Upgrade")
        self.assertEqual(len(upgrades), 1)
        self.assertEqual(upgrades[0].attrib["Id"], "$(OtherArchitectureUpgradeCode)")
        versions = self.elements("UpgradeVersion")
        self.assertEqual(len(versions), 1)
        self.assertEqual(
            versions[0].attrib,
            {
                "Minimum": "0.0.0",
                "IncludeMinimum": "yes",
                "OnlyDetect": "yes",
                "Property": "OTHERARCHITECTUREDETECTED",
            },
        )
        launches = self.elements("Launch")
        self.assertEqual(len(launches), 1)
        self.assertEqual(
            launches[0].attrib["Condition"],
            "Installed OR NOT OTHERARCHITECTUREDETECTED",
        )
        self.assertIn("OtherArchitectureName", launches[0].attrib["Message"])

    def test_architecture_contract_covers_x64_and_arm64ec_payloads(self) -> None:
        for token in (
            "[ValidateSet('x64', 'arm64ec')]",
            "Vst3Directory = 'x86_64-win'",
            "MsiArchitecture = 'x64'",
            "Vst3Directory = 'arm64ec-win'",
            "MsiArchitecture = 'arm64'",
            "Artifact = 'arm64ec'",
            r"8664 machine \(x64\) \(ARM64X\)",
            "Expected Arm64 MSI template",
            "MSI contains a non-64-bit component",
            "foreach ($relativeUpdater in $updaterRelativePaths)",
            "foreach ($relativePe in $payloadContract.PortableExecutablePaths)",
            "Assert-PeArchitecture (Join-Path $stagedBundle $relativePe)",
        ):
            self.assertIn(token, self.script)

    def test_moduleinfo_identity_and_mutations_are_fail_closed(self) -> None:
        contract_start = self.script.index("function Test-ModuleInfoContract")
        mutation_end = self.script.index("function Normalize-Guid", contract_start)
        contract = self.script[contract_start:mutation_end]
        for token in (
            "UTF8Encoding]::new($false, $true)",
            "$options.AllowTrailingCommas = $true",
            "JsonCommentHandling]::Disallow",
            "$options.MaxDepth = 32",
            "Assert-JsonTreeHasUniqueProperties",
            "Assert-ExactJsonString $root 'Name' $ProductName",
            "Assert-ExactJsonString $root 'Version' $ProductVersion",
            "Assert-ExactJsonString $factoryInfo 'Vendor' $Manufacturer",
            "$actualClasses.Count -eq $ExpectedClasses.Count",
            "Assert-ExactJsonString $class 'Name' $ProductName",
            "Assert-ExactJsonString $class 'Vendor' $Manufacturer",
            "Assert-ExactJsonString $class 'Version' $ProductVersion",
            "$actualIdentities.SetEquals($expectedIdentities)",
            "Description = 'product'",
            "Description = 'manufacturer'",
            "Description = 'version'",
            "Description = 'class CID'",
            "Description = 'class category'",
            "Description = 'extra class'",
            "$juceTrailingCommaJson",
            '"Vendor":"Policy Vendor",}',
            "return 12",
        ):
            self.assertIn(token, contract)
        validation = self.script.index("$moduleInfoContract = Test-ModuleInfoContract")
        staging = self.script.index("Copy-Snapshot $initialSnapshot")
        self.assertLess(validation, staging)
        for token in (
            "moduleInfoIdentityValidated = $true",
            "classIdentities = @($moduleInfoContract.ClassIdentities)",
            "sha256 = $moduleInfoContract.Sha256",
            "schemaVersion = 2",
        ):
            self.assertIn(token, self.script)

    def test_complete_payload_pe_classification_and_mutations_are_fail_closed(self) -> None:
        classifier_start = self.script.index("function Get-PeImageClassification")
        classifier_end = self.script.index("function Assert-JsonTreeHasUniqueProperties")
        classifier = self.script[classifier_start:classifier_end]
        for token in (
            "$dosMagic[0] -ne 0x4d",
            "$peMagic[0] -eq 0x50",
            "MZ payload does not contain a valid PE signature",
            "$forbiddenExecutableExtensions",
            "'.ps1'",
            "'.bat'",
            "'.vbs'",
            "'.py'",
            "'.msi'",
            "$peRequiredExtensions = @('.dll', '.exe', '.vst3')",
            "Executable payload extension does not contain an MZ/PE image",
            "if ($classification.IsPortableExecutable)",
            "-AllowUnsigned payloads must contain only the exact primary VST3 PE",
            "Production payloads require exactly one updater-like PE helper",
        ):
            self.assertIn(token, classifier)
        mutation_start = self.script.index("function Invoke-InstallerPolicyMutationTests")
        mutation_end = self.script.index("function Normalize-Guid", mutation_start)
        mutations = self.script[mutation_start:mutation_end]
        for token in (
            "opaque-resource.dat",
            "unsigned renamed PE helper",
            "non-MZ .exe",
            "embedded script",
            "unsigned hidden PE helper",
            "second updater-like PE",
            "malformed MZ image",
        ):
            self.assertIn(token, mutations)
        self.assertIn(
            "[string[]] $signableRelativePaths = @($payloadContract.PortableExecutablePaths)",
            self.script,
        )
        self.assertIn(
            "everyPortableExecutableArchitectureValidated = $true", self.script
        )
        self.assertIn("signedPortableExecutablePaths", self.script)

    def test_primary_pe_version_resource_binds_binary_identity(self) -> None:
        resource_start = self.script.index(
            "function Test-PluginVersionResourceContract"
        )
        resource_end = self.script.index(
            "function Assert-PolicyMutationRejected", resource_start
        )
        resource = self.script[resource_start:resource_end]
        for token in (
            "[System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path)",
            "$versionInfo.FileVersion -ceq $ProductVersion",
            "$versionInfo.ProductVersion -ceq $ProductVersion",
            "$versionInfo.CompanyName -ceq $Manufacturer",
            "$versionInfo.ProductName -ceq $ProductName",
            "$versionInfo.FileDescription -ceq $ProductName",
        ):
            self.assertIn(token, resource)
        validation = self.script.index(
            "$pluginVersionResourceContract = Test-PluginVersionResourceContract"
        )
        payload_sign = self.script.index("foreach ($relative in $signableRelativePaths)")
        self.assertLess(validation, payload_sign)
        for token in (
            "primary VST3 version resource",
            "primary VST3 company resource",
            "primary VST3 product resource",
            "$pluginVersionResourceMutationTestCount = 3",
            "pluginVersionResourceValidated = $true",
            "fileVersion = $pluginVersionResourceContract.FileVersion",
            "productVersion = $pluginVersionResourceContract.ProductVersion",
            "companyName = $pluginVersionResourceContract.CompanyName",
            "productName = $pluginVersionResourceContract.ProductName",
            "fileDescription = $pluginVersionResourceContract.FileDescription",
        ):
            self.assertIn(token, self.script)

    def test_msi_identity_language_and_launch_conditions_match_runtime(self) -> None:
        for token in (
            "$properties['ProductName'] -ceq $DisplayName",
            "$properties['Manufacturer'] -ceq $Manufacturer",
            "$properties['ProductLanguage'] -ceq '1033'",
            "$properties['MSIDEPLOYMENTCOMPLIANT'] -ceq '1'",
            "displayName = $msiContract.ProductName",
            "msiDeploymentCompliant = $msiContract.DeploymentCompliant",
            "$secureCustomProperties.SetEquals($expectedSecureCustomProperties)",
            "secureCustomProperties = @($msiContract.SecureCustomProperties)",
            "Malformed or duplicate MSI Property row",
            "Duplicate MSI LaunchCondition row",
            "INSTALLEDORNOTWIX_DOWNGRADE_DETECTED",
            "INSTALLEDORNOTOTHERARCHITECTUREDETECTED",
            "$launchConditions.SetEquals($expectedLaunchConditions)",
            "MSI LaunchCondition table is not the exact downgrade/architecture contract",
        ):
            self.assertIn(token, self.script)

    def test_production_updater_version_resource_is_identity_bound(self) -> None:
        for token in (
            "function Test-UpdaterVersionResourceContract",
            "$versionInfo.FileVersion -ceq $ProductVersion",
            "$versionInfo.ProductVersion -ceq $ProductVersion",
            "$versionInfo.CompanyName -ceq $Manufacturer",
            "$versionInfo.ProductName -ceq $ProductName",
            '$versionInfo.FileDescription -ceq "$ProductName Updater"',
            '$versionInfo.InternalName -ceq "$($ProductName)Updater"',
            '$versionInfo.OriginalFilename -ceq "$($ProductName)Updater.exe"',
            "updaterVersionResource = if ($AllowUnsigned)",
            "mutationTests = $updaterVersionResourceMutationTestCount",
        ):
            self.assertIn(token, self.script)
        self.assertIn("$updaterVersionResourceMutationTestCount = 3", self.script)

    def test_production_requires_host_test_and_snapshot_bound_updater(self) -> None:
        production = self.script[
            self.script.index("if ($AllowUnsigned)") :
            self.script.index("$outputRoot = Get-FullPath")
        ]
        for token in (
            "Production mode requires -SourceCommit as exactly 40 lowercase hexadecimal characters",
            "Production mode requires -HostTestPath",
            "$UpdaterPath.Count -eq 1",
            "Production packages require exactly the product updater",
        ):
            self.assertIn(token, production)
        self.assertIn("sourceCommit = if ($AllowUnsigned) { $null } else { $SourceCommit }", self.script)
        self.assertIn(
            "$payloadContract.PortableExecutablePaths -ccontains $relativeUpdater",
            self.script,
        )
        self.assertIn("payloadClassificationValidated = $true", self.script)

    def test_signing_is_fail_closed_and_precedes_msi_signing(self) -> None:
        for token in (
            "Production mode requires exactly one of -CertificateThumbprint or -CertificateSubject",
            "Production mode requires -TimestampUrl",
            "Production mode requires the exact 64-hex -ExpectedSignerSha256 compiled into the updater",
            "Selected signing certificate does not match the SHA-256 fingerprint compiled into the updater",
            "Production packages require exactly the product updater",
            "Invoke-UpdaterBuildContract",
            "--validate-build-contract",
            "--challenge",
            "--response-pipe",
            "--parent-process-id",
            "--manufacturer",
            "--github-owner",
            "--github-repository",
            "RandomNumberGenerator]::Fill",
            "PipeOptions]::CurrentUserOnly",
            "NamedPipeServerStream]::new",
            "WaitForConnectionAsync",
            "GetNamedPipeClientProcessId",
            "Assert-NamedPipeClientProcess $responsePipe $process",
            "$clientProcessId -eq [uint32]$ExpectedProcess.Id",
            "whykiki.windows-updater-build-contract",
            "[byte[]]::new($expectedBytes.Length + 1)",
            "returned the wrong schema, challenge or compiled build identity",
            "Staged updater rejected its exact production build contract",
            "'/fd', 'SHA256', '/td', 'SHA256', '/tr'",
            "signtool.exe",
            "-UNSIGNED-NOT-FOR-DISTRIBUTION",
            "Do not pass signing options together with -AllowUnsigned",
        ):
            self.assertIn(token, self.script)
        payload_sign = self.script.index("foreach ($relative in $signableRelativePaths)")
        wix_build = self.script.index("'build', $script:PackageSource")
        msi_sign = self.script.index("Invoke-AuthenticodeSign $msiPath")
        self.assertLess(payload_sign, wix_build)
        self.assertLess(wix_build, msi_sign)
        for token in (
            "Resolve-SigningCertificate",
            "Signing identity must resolve to exactly one certificate",
            "Unexpected signer certificate after signing",
            "TimeStamperCertificate",
            "signerCertificateSha256",
            "updaterSignerPinSha256",
        ):
            self.assertIn(token, self.script)
        self.assertNotIn("@('/n',", self.script)
        self.assertLess(
            self.script.index("$signerCertificateSha256 -ceq $ExpectedSignerSha256"),
            payload_sign,
        )
        self.assertLess(self.script.index("Invoke-UpdaterBuildContract (Join-Path"), payload_sign)
        contract_start = self.script.index("function Invoke-UpdaterBuildContract")
        contract_end = self.script.index("function Invoke-AdministrativeExtraction", contract_start)
        contract = self.script[contract_start:contract_end]
        self.assertNotIn("ReadToEnd", contract)
        self.assertNotIn("RedirectStandardOutput", contract)
        self.assertNotIn("RedirectStandardError", contract)
        self.assertLess(contract.index("$connectionTask = $responsePipe.WaitForConnectionAsync()"),
                        contract.index("Process]::Start($startInfo)"))
        self.assertLess(contract.index("Assert-NamedPipeClientProcess $responsePipe $process"),
                        contract.index("$responsePipe.ReadAsync("))
        self.assertLess(contract.index("$byteDifference -eq 0"),
                        contract.index("$exitCode -eq 0"))

    def test_build_contract_rejects_exit_only_wrong_peer_and_noncanonical_bytes(self) -> None:
        peer_check = self.script.index(
            "$clientProcessId -eq [uint32]$ExpectedProcess.Id"
        )
        contract_start = self.script.index("function Invoke-UpdaterBuildContract")
        contract_end = self.script.index(
            "function Invoke-AdministrativeExtraction", contract_start
        )
        contract = self.script[contract_start:contract_end]
        self.assertLess(contract.index("Assert-NamedPipeClientProcess"),
                        contract.index("$responsePipe.ReadAsync("))
        self.assertLess(contract.index("$receivedCount -eq $expectedBytes.Length"),
                        contract.index("$exitCode -eq 0"))
        self.assertLess(contract.index("$byteDifference -eq 0"),
                        contract.index("$exitCode -eq 0"))
        self.assertIn("$expectedBytes.Length + 1", contract)
        self.assertIn("CurrentUserOnly", contract)
        self.assertIn("RandomNumberGenerator]::Fill", self.script)
        self.assertGreater(peer_check, self.script.index("GetNamedPipeClientProcessId"))

    def test_msi_validation_and_host_hook_are_mandatory_contracts(self) -> None:
        for token in (
            "Invoke-Wix @('msi', 'validate'",
            "WindowsInstaller.Installer",
            "InstallExecuteSequence",
            "RemoveExistingProducts",
            "WIX_DOWNGRADE_DETECTED",
            "OTHERARCHITECTUREDETECTED",
            "MSI has $($upgradeRows.Count) Upgrade rows; expected exactly 3",
            "Assert-MsiUpgradeRow $upgradeByProperty['WIX_UPGRADE_DETECTED']",
            "'0.0.0' '' '' 258 'OTHERARCHITECTUREDETECTED'",
            "Invoke-AdministrativeExtraction",
            "Get-FileHash",
            "$HostTestPath",
            "VST3 host test rejected the extracted bundle",
            "administrativeExtractionHashMatch = $true",
            "System.Diagnostics.ProcessStartInfo",
            "timestampProtocol = if ($AllowUnsigned) { $null } else { 'RFC3161-SHA256' }",
            "timestampUrlSha256",
            "nuGetConfigSha256",
            "Get-ComProperty $Installer 'SummaryInformation'",
            "$row.Fields[0]",
            "AdministrativeExtractionTimeoutSeconds",
            "HostTestTimeoutSeconds",
            "WaitForExit($TimeoutSeconds * 1000)",
            "Stop-TimedOutProcess",
        ):
            self.assertIn(token, self.script)
        self.assertNotIn("timestampUrl = if", self.script)

    def test_versions_and_release_candidates_are_canonical_and_non_overwriting(self) -> None:
        self.assertIn(
            "^(0|[1-9][0-9]{0,2})\\.(0|[1-9][0-9]{0,2})\\.(0|[1-9][0-9]{0,4})$",
            self.script,
        )
        for token in (
            "MSI versions require major/minor <= 255 and build <= 65535",
            "Refusing to overwrite an existing installer candidate",
            "[System.IO.Directory]::Move($candidate, $finalCandidate)",
        ):
            self.assertIn(token, self.script)

    def test_documentation_calls_out_distribution_and_osmf_constraints(self) -> None:
        for token in (
            "UNSIGNED-NOT-FOR-DISTRIBUTION",
            "Open Source Maintenance Fee",
            "CommonFiles6432Folder",
            "afterInstallInitialize",
            "ARM64EC",
            "RFC 3161",
            "keine Custom Actions",
            "<Product>-<Version>-Windows-arm64ec.msi",
        ):
            self.assertIn(token, self.docs)


if __name__ == "__main__":
    unittest.main()
