#!/usr/bin/env python3
"""Static and executable contracts for the signed cross-platform release workflow."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import re
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PRODUCT = ROOT.name
CI_WORKFLOW = "build.yml" if PRODUCT == "SubLab808" else "ci.yml"
SIGNER = "A1" * 32
APPLICATION_SIGNER = "B2" * 32
INSTALLER_SIGNER = "C3" * 32
TAG_COMMIT = "1" * 40


def load_module(name: str, filename: str):
    path = ROOT / "scripts" / filename
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load release helper: {filename}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WindowsReleaseContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.release = (ROOT / ".github" / "workflows" / "windows-release.yml").read_text(
            encoding="utf-8"
        )
        cls.ci = (ROOT / ".github" / "workflows" / CI_WORKFLOW).read_text(encoding="utf-8")
        cls.docs = (ROOT / "WINDOWS_RELEASE.md").read_text(encoding="utf-8")
        cls.validator = load_module(
            "windows_release_validator", "validate-windows-release-assets.py"
        )
        cls.macos_validator = load_module("macos_release_assets", "macos-release-assets.py")

    def test_dispatch_is_confirmed_default_branch_and_existing_tag_only(self) -> None:
        self.assertIn("workflow_dispatch:", self.release)
        self.assertNotRegex(self.release, r"(?m)^  (push|pull_request|release|schedule):")
        for token in (
            "confirm_release:",
            "inputs.confirm_release == true",
            "github.event.repository.default_branch",
            "ref: ${{ inputs.tag }}",
            "persist-credentials: false",
            "git show-ref --verify --quiet",
            "git ls-remote --exit-code origin",
            "refs/tags/$tag^{commit}",
            "git merge-base --is-ancestor",
            "Tag version does not exactly match CMAKE_PROJECT_VERSION",
        ):
            self.assertIn(token, self.release)
        self.assertGreaterEqual(
            self.release.count("^v(0|[1-9][0-9]{0,2})"), 2
        )
        self.assertIn(f"group: {PRODUCT}-windows-release\n", self.release)
        self.assertNotIn("windows-release-${{ inputs.tag }}", self.release)

    def test_native_architectures_and_production_build_are_separate(self) -> None:
        for token in (
            "runner: windows-2022",
            "runner: windows-11-vs2026-arm",
            "platform: x64",
            "platform: ARM64EC",
            "artifact_arch: x64",
            "artifact_arch: arm64ec",
            f"-D{PRODUCT.upper()}_WINDOWS_UPDATER_SIGNER_SHA256:STRING=$env:WK_SIGNER_SHA256",
            f"{PRODUCT}_VST3 {PRODUCT}WindowsUpdater",
            f"Contents/Helpers/{PRODUCT}Updater.exe",
            "WindowsUpdaterPolicyTests",
            "WindowsUpdaterSelfTests",
            "HostTests",
            "PresetTests",
        ):
            self.assertIn(token, self.release)

    def test_pfx_is_ephemeral_and_leaf_pin_is_fail_closed(self) -> None:
        for token in (
            "secrets.WINDOWS_CODE_SIGNING_PFX_BASE64",
            "secrets.WINDOWS_CODE_SIGNING_PFX_PASSWORD",
            "vars.WINDOWS_CODE_SIGNING_CERT_SHA256",
            "vars.WINDOWS_RFC3161_TIMESTAMP_URL",
            "Import-PfxCertificate",
            "X509EnhancedKeyUsageExtension",
            "1.3.6.1.5.5.7.3.3",
            "HashAlgorithmName]::SHA256",
            "$actualPin -cne $env:WK_SIGNER_SHA256",
            "PFX must contain exactly one private-key leaf certificate",
            "SetAccessRuleProtection($true, $false)",
            "FileSystemRights]::FullControl",
            "if: always()",
            "Remove-Item -LiteralPath $env:WK_PFX_PATH -Force",
            "foreach ($certificate in @($store.Certificates)) { $store.Remove($certificate) }",
            "if ($null -ne $store) { $store.Dispose() }",
            "Ephemeral certificate store cleanup failed",
        ):
            self.assertIn(token, self.release)
        self.assertNotIn("Write-Host $env:PFX", self.release)

    def test_packager_receives_complete_signed_production_contract(self) -> None:
        for token in (
            "scripts/build-windows-installer.ps1",
            "-UpdaterPath $updater",
            "-SourceCommit $env:WK_TAG_COMMIT",
            "-ExpectedSignerSha256 $env:WK_SIGNER_SHA256",
            "-HostTestPath $hostTest",
            "-SignToolPath $signTool",
            "-CertificateThumbprint $env:WK_CERT_THUMBPRINT",
            "-CertificateStoreName $env:WK_CERT_STORE_NAME",
            "-TimestampUrl $env:TIMESTAMP_URL",
            "scripts/validate-windows-release-assets.py",
            "--source-commit $env:WK_TAG_COMMIT",
        ):
            self.assertIn(token, self.release)
        self.assertNotIn("-AllowUnsigned", self.release)

    def test_release_is_staged_complete_then_published_once(self) -> None:
        self.assertEqual(self.release.count("contents: write"), 1)
        for token in (
            "needs: [build-windows, build-macos]",
            "actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093",
            "merge-multiple: true",
            "github.run_attempt",
            "--architecture x64 --architecture arm64ec",
            '--source-commit "$tag_commit"',
            "SHA256SUMS.txt",
            "created_release_id=\"$(gh api --method POST",
            "-f tag_name=\"$tag\" -f target_commitish=\"$tag_commit\"",
            "gh release upload",
            "gh api --method PATCH",
            "-F draft=false -F prerelease=false -f make_latest=true",
            "releases/latest",
            "releases/latest\" --jq '.tag_name'",
            "verify_origin_tag",
            "latest_tag=",
            "baseline_latest_id=",
            "read_latest_state()",
            "gh api graphql",
            "current_latest_state=",
            "test \"$current_latest_state\" = \"$latest_state\"",
            "tuple(map(int",
            ".assets[] | select(.name",
            ".digest",
            "cleanup_draft",
        ):
            self.assertIn(token, self.release)
        cleanup_start = self.release.index("cleanup_draft()")
        cleanup_end = self.release.index("trap cleanup_draft EXIT", cleanup_start)
        cleanup = self.release[cleanup_start:cleanup_end]
        self.assertIn("releases/$created_release_id", cleanup)
        self.assertIn("draft_state=", cleanup)
        self.assertIn('[[ "$draft_state" == true ]]', cleanup)
        self.assertNotIn("releases?per_page", cleanup)
        self.assertNotIn("tag_name", cleanup)
        self.assertGreaterEqual(self.release.count("verify_origin_tag\n"), 2)
        self.assertNotIn('releases/latest" --jq \'.id\' 2>/dev/null || true', self.release)
        self.assertGreaterEqual(self.release.count("${{ github.run_attempt }}"), 4)
        post_publish = self.release.index("published=true")
        self.assertIn("--jq '.tag_name'", self.release[post_publish:])
        for architecture in ("x64", "arm64ec"):
            self.assertIn(
                f'release-assets/windows/{PRODUCT}-$version-Windows-{architecture}.msi',
                self.release,
            )
            self.assertIn(
                f'release-assets/windows/{PRODUCT}-$version-Windows-{architecture}.evidence.json',
                self.release,
            )

    def test_macos_candidate_is_signed_notarized_and_required_for_publish(self) -> None:
        product_prefix = PRODUCT.upper()
        for token in (
            "build-macos:",
            "runs-on: macos-15",
            "needs: [build-windows, build-macos]",
            "secrets.MACOS_DEVELOPER_ID_APPLICATION_P12_BASE64",
            "secrets.MACOS_DEVELOPER_ID_APPLICATION_P12_PASSWORD",
            "secrets.MACOS_DEVELOPER_ID_INSTALLER_P12_BASE64",
            "secrets.MACOS_DEVELOPER_ID_INSTALLER_P12_PASSWORD",
            "secrets.MACOS_NOTARY_PRIVATE_KEY_P8_BASE64",
            "vars.MACOS_DEVELOPER_ID_APPLICATION_CERT_SHA256",
            "vars.MACOS_DEVELOPER_ID_INSTALLER_CERT_SHA256",
            "security create-keychain",
            "security set-key-partition-list",
            "notarytool store-credentials",
            '--keychain "$temporary_keychain"',
            'rm -f -- "$app_p12" "$installer_p12" "$notary_key"',
            '[[ ! -e "$credential_file" ]]',
            "unset APPLICATION_P12_BASE64 APPLICATION_P12_PASSWORD",
            f"export {product_prefix}_NOTARY_KEYCHAIN=\"$temporary_keychain\"",
            './scripts/package-release.sh Release "$WK_RELEASE_VERSION"',
            "scripts/macos-release-assets.py prepare",
            "scripts/macos-release-assets.py validate",
            "pkgutil --check-signature",
            "Signed with a trusted timestamp",
            "codesign --verify --deep --strict",
            "--extract-certificates",
            "xcrun stapler validate",
            "spctl --assess --type install",
            "spctl --assess --type execute",
            "test \"$architectures\" = 'arm64 x86_64'",
            "security delete-keychain",
            '"$RUNNER_TEMP"/whykiki-release.*)',
            '[[ ! -e "$credential_dir" ]]',
            'cleanup_failed=true',
            f"release-assets/macos/{PRODUCT}-$version-macOS-universal.pkg",
            f"release-assets/macos/{PRODUCT}-$version-macOS-universal-VST3.zip",
            f"release-assets/macos/{PRODUCT}-$version-macOS-universal.evidence.json",
            f"release-assets/{PRODUCT}-$version-SHA256SUMS.txt",
        ):
            self.assertIn(token, self.release)
        self.assertEqual(self.release.count("retention-days: 1"), 2)
        self.assertEqual(self.release.count("APPLICATION_SIGNER_SHA256:"), 2)
        self.assertEqual(self.release.count("INSTALLER_SIGNER_SHA256:"), 2)
        self.assertNotIn(f"{PRODUCT}-$version-Windows-SHA256SUMS.txt", self.release)
        self.assertLess(
            self.release.index("unset APPLICATION_P12_BASE64"),
            self.release.index('./scripts/package-release.sh Release "$WK_RELEASE_VERSION"'),
        )
        publish = self.release[self.release.index("publish-release:") :]
        self.assertIn("macOS-universal", publish)
        self.assertIn("Windows-arm64ec", publish)

    def test_all_external_actions_are_full_commit_pinned(self) -> None:
        references = re.findall(r"(?m)^\s*-?\s*uses:\s*[^@\s]+@([^\s#]+)", self.release)
        self.assertTrue(references)
        self.assertTrue(all(re.fullmatch(r"[0-9a-f]{40}", reference) for reference in references))

    def test_unsigned_ci_vst3_file_and_container_names_are_exact(self) -> None:
        expected = f"{PRODUCT}-Windows-${{{{ matrix.artifact_arch }}}}-VST3-UNSIGNED-NOT-FOR-DISTRIBUTION"
        self.assertGreaterEqual(self.ci.count(expected), 2)
        self.assertNotIn(f"{PRODUCT}-VST3-Windows-", self.ci)

    def _write_candidate(self, directory: pathlib.Path, architecture: str) -> pathlib.Path:
        version = "1.2.3"
        base = f"{PRODUCT}-{version}-Windows-{architecture}"
        msi = directory / f"{base}.msi"
        msi.write_bytes(f"signed-msi-{architecture}".encode("ascii"))
        package_config = json.loads(
            (ROOT / "Installer/Windows/package-config.json").read_text(encoding="utf-8")
        )
        other_architecture = "arm64ec" if architecture == "x64" else "x64"
        evidence = {
            "schemaVersion": 2,
            "artifactStatus": "SIGNED",
            "product": PRODUCT,
            "version": version,
            "sourceCommit": TAG_COMMIT,
            "payloadArchitecture": architecture,
            "msiArchitecture": "x64" if architecture == "x64" else "arm64",
            "wixVersion": "6.0.2",
            "wixToolManifestSha256": "E5" * 32,
            "nuGetConfigSha256": "F6" * 32,
            "productCode": "12345678-1234-4234-8234-123456789ABC",
            "upgradeCode": package_config["upgradeCodes"][architecture],
            "otherArchitectureUpgradeCode": package_config["upgradeCodes"][other_architecture],
            "msiFile": msi.name,
            "msiSha256": hashlib.sha256(msi.read_bytes()).hexdigest().upper(),
            "signed": True,
            "signerCertificateSha256": SIGNER,
            "updaterSignerPinSha256": SIGNER,
            "signingDigest": "SHA256",
            "timestampProtocol": "RFC3161-SHA256",
            "timestampUrlSha256": "A7" * 32,
            "updaterPaths": [f"Contents\\Helpers\\{PRODUCT}Updater.exe"],
            "moduleInfo": {
                "path": "Contents\\Resources\\moduleinfo.json",
                "sha256": "C3" * 32,
                "name": PRODUCT,
                "manufacturer": "Whykiki Audio",
                "version": version,
                "classIdentities": [
                    f"{entry['cid']}|{entry['category']}"
                    for entry in package_config["vst3Classes"]
                ],
            },
            "pluginVersionResource": {
                "fileVersion": version,
                "productVersion": version,
                "companyName": "Whykiki Audio",
                "productName": PRODUCT,
                "fileDescription": PRODUCT,
                "mutationTests": 3,
            },
            "updaterVersionResource": {
                "fileVersion": version,
                "productVersion": version,
                "companyName": "Whykiki Audio",
                "productName": PRODUCT,
                "fileDescription": f"{PRODUCT} Updater",
                "internalName": f"{PRODUCT}Updater",
                "originalFilename": f"{PRODUCT}Updater.exe",
                "mutationTests": 3,
            },
            "payloadClassification": {
                "portableExecutablePaths": [
                    f"Contents\\{'x86_64-win' if architecture == 'x64' else 'arm64ec-win'}\\{PRODUCT}.vst3",
                    f"Contents\\Helpers\\{PRODUCT}Updater.exe",
                ],
                "helperPortableExecutablePaths": [f"Contents\\Helpers\\{PRODUCT}Updater.exe"],
                "updaterLikePortableExecutablePaths": [f"Contents\\Helpers\\{PRODUCT}Updater.exe"],
                "forbiddenExecutableExtensions": 0,
                "malformedExecutableExtensions": 0,
                "everyPortableExecutableArchitectureValidated": True,
                "signedPortableExecutablePaths": [
                    f"Contents\\{'x86_64-win' if architecture == 'x64' else 'arm64ec-win'}\\{PRODUCT}.vst3",
                    f"Contents\\Helpers\\{PRODUCT}Updater.exe",
                ],
            },
            "payloadFiles": [
                {
                    "path": f"Contents\\Helpers\\{PRODUCT}Updater.exe",
                    "size": 1,
                    "sha256": "B2" * 32,
                },
                {
                    "path": f"Contents\\{'x86_64-win' if architecture == 'x64' else 'arm64ec-win'}\\{PRODUCT}.vst3",
                    "size": 1,
                    "sha256": "D4" * 32,
                },
                {
                    "path": "Contents\\Resources\\moduleinfo.json",
                    "size": 1,
                    "sha256": "C3" * 32,
                },
            ],
            "validation": {
                "policyMutationTests": 12,
                "moduleInfoIdentityValidated": True,
                "pluginVersionResourceValidated": True,
                "updaterVersionResourceValidated": True,
                "payloadClassificationValidated": True,
                "wixIce": True,
                "customActions": 0,
                "forbiddenSideEffectTables": 0,
                "directoryGraphValidated": True,
                "componentContainmentValidated": True,
                "fileComponentReferencesValidated": True,
                "forbiddenSequenceActions": 0,
                "administrativeExtractionHashMatch": True,
                "administrativeImageLayoutValidated": True,
                "hostTestRan": True,
                "displayName": (
                    f"{PRODUCT} VST3 - Windows x64"
                    if architecture == "x64"
                    else f"{PRODUCT} VST3 - Windows on Arm (ARM64EC)"
                ),
                "manufacturer": "Whykiki Audio",
                "productLanguage": "1033",
                "msiDeploymentCompliant": "1",
                "secureCustomProperties": [
                    "OTHERARCHITECTUREDETECTED",
                    "WIX_DOWNGRADE_DETECTED",
                    "WIX_UPGRADE_DETECTED",
                ],
                "launchConditions": [
                    "INSTALLEDORNOTOTHERARCHITECTUREDETECTED",
                    "INSTALLEDORNOTWIX_DOWNGRADE_DETECTED",
                ],
            },
        }
        evidence_path = directory / f"{base}.evidence.json"
        evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
        return evidence_path

    def test_asset_validator_accepts_only_complete_hash_bound_candidates(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            evidence_path = self._write_candidate(directory, "x64")
            self.validator.validate_assets(
                directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, ("x64",)
            )
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["msiSha256"] = "00" * 32
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, ("x64",)
                )

    def test_asset_validator_rejects_missing_other_architecture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self._write_candidate(directory, "x64")
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, ("x64", "arm64ec")
                )

    def test_asset_validator_rejects_a_different_source_commit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self._write_candidate(directory, "x64")
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    directory, PRODUCT, "1.2.3", "0" * 40, SIGNER, ("x64",)
                )

    def test_asset_validator_rejects_out_of_range_msi_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    pathlib.Path(temporary), PRODUCT, "256.0.0", TAG_COMMIT, SIGNER, ("x64",)
                )

    def test_asset_validator_rejects_incomplete_updater_and_msi_policy_evidence(self) -> None:
        mutations = (
            ("updaterVersionResource", "originalFilename", "WrongUpdater.exe"),
            ("validation", "msiDeploymentCompliant", "0"),
            ("validation", "secureCustomProperties", ["WIX_UPGRADE_DETECTED"]),
            ("validation", "displayName", PRODUCT),
        )
        for section, key, value in mutations:
            with self.subTest(section=section, key=key), tempfile.TemporaryDirectory() as temporary:
                directory = pathlib.Path(temporary)
                evidence_path = self._write_candidate(directory, "x64")
                evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
                evidence[section][key] = value
                evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
                with self.assertRaises(self.validator.ContractError):
                    self.validator.validate_assets(
                        directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, ("x64",)
                    )

    def _write_macos_pipeline_candidate(self, candidate: pathlib.Path) -> None:
        version = "1.2.3"
        package_name = f"{PRODUCT}-{version}-macOS-universal.pkg"
        archive_name = f"{PRODUCT}-{version}-macOS-universal-VST3.zip"
        package = candidate / package_name
        archive = candidate / archive_name
        package.write_bytes(b"signed and notarized package fixture")
        archive.write_bytes(b"signed and notarized VST3 archive fixture")
        source_sha = "2" * 64
        source_manifest = {
            "schema": 1,
            "repositories": [
                {"path": ".", "commit": TAG_COMMIT, "dirty": False},
                {"path": "external/JUCE", "commit": "3" * 40, "dirty": False},
            ],
            "files": [],
            "source_sha256": source_sha,
        }
        source_path = candidate / "source-manifest.json"
        source_path.write_text(json.dumps(source_manifest), encoding="utf-8")

        def digest(path: pathlib.Path) -> str:
            return hashlib.sha256(path.read_bytes()).hexdigest()

        release_manifest = {
            "schema": 1,
            "version": version,
            "configuration": "Release",
            "commit": TAG_COMMIT,
            "source_sha256": source_sha,
            "dirty": False,
            "built_binary_sha256": "4" * 64,
            "packaged_binary_sha256": "5" * 64,
            "application_signed": True,
            "installer_signed": True,
            "notarized": True,
            "notary_submission_id": "00000000-0000-4000-8000-000000000001",
            "verification": sorted(self.macos_validator.REQUIRED_VERIFICATION),
            "artifacts": {
                package_name: digest(package),
                archive_name: digest(archive),
                "source-manifest.json": digest(source_path),
            },
        }
        (candidate / "release-manifest.json").write_text(
            json.dumps(release_manifest), encoding="utf-8"
        )

    def test_macos_asset_helper_binds_source_signers_notarization_and_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            candidate = root / "candidate"
            candidate.mkdir()
            output = root / "release-assets"
            self._write_macos_pipeline_candidate(candidate)
            self.macos_validator.prepare(
                candidate,
                output,
                PRODUCT,
                "1.2.3",
                TAG_COMMIT,
                APPLICATION_SIGNER,
                INSTALLER_SIGNER,
            )
            self.macos_validator.validate(
                output,
                PRODUCT,
                "1.2.3",
                TAG_COMMIT,
                APPLICATION_SIGNER,
                INSTALLER_SIGNER,
            )
            archive = output / f"{PRODUCT}-1.2.3-macOS-universal-VST3.zip"
            archive.write_bytes(b"tampered")
            with self.assertRaises(self.macos_validator.ContractError):
                self.macos_validator.validate(
                    output,
                    PRODUCT,
                    "1.2.3",
                    TAG_COMMIT,
                    APPLICATION_SIGNER,
                    INSTALLER_SIGNER,
                )

    def test_macos_asset_helper_rejects_dirty_or_unsafe_source_evidence(self) -> None:
        for mutation in ("dirty", "unsafe_path"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                candidate = root / "candidate"
                candidate.mkdir()
                self._write_macos_pipeline_candidate(candidate)
                source_path = candidate / "source-manifest.json"
                source = json.loads(source_path.read_text(encoding="utf-8"))
                if mutation == "dirty":
                    source["repositories"][1]["dirty"] = True
                else:
                    source["repositories"][1]["path"] = "../JUCE"
                source_path.write_text(json.dumps(source), encoding="utf-8")
                manifest_path = candidate / "release-manifest.json"
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                manifest["artifacts"]["source-manifest.json"] = hashlib.sha256(
                    source_path.read_bytes()
                ).hexdigest()
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
                with self.assertRaises(self.macos_validator.ContractError):
                    self.macos_validator.prepare(
                        candidate,
                        root / "output",
                        PRODUCT,
                        "1.2.3",
                        TAG_COMMIT,
                        APPLICATION_SIGNER,
                        INSTALLER_SIGNER,
                    )

    def test_documentation_names_required_configuration_and_no_release_claim(self) -> None:
        for token in (
            "WINDOWS_CODE_SIGNING_PFX_BASE64",
            "WINDOWS_CODE_SIGNING_PFX_PASSWORD",
            "WINDOWS_CODE_SIGNING_CERT_SHA256",
            "WINDOWS_RFC3161_TIMESTAMP_URL",
            "MACOS_DEVELOPER_ID_APPLICATION_P12_BASE64",
            "MACOS_DEVELOPER_ID_INSTALLER_P12_BASE64",
            "MACOS_NOTARY_PRIVATE_KEY_P8_BASE64",
            "MACOS_DEVELOPER_ID_APPLICATION_CERT_SHA256",
            "MACOS_DEVELOPER_ID_INSTALLER_CERT_SHA256",
            "confirm_release",
            "ARM64EC",
            "Draft",
        ):
            self.assertIn(token, self.docs)
        self.assertIn("veröffentlicht jetzt nichts", self.docs)


if __name__ == "__main__":
    unittest.main()
