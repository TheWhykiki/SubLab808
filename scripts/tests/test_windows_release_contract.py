#!/usr/bin/env python3
"""Static and executable contracts for the signed cross-platform release workflow."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest
import datetime as dt


ROOT = pathlib.Path(__file__).resolve().parents[2]
PRODUCT = json.loads((ROOT / "release/product.json").read_text(encoding="utf-8"))["productName"]
CI_WORKFLOW = "build.yml" if PRODUCT == "SubLab808" else "ci.yml"
SIGNER = "1.3.6.1.4.1.311.97.100.200.300.400"
NEXT_SIGNER = "1.3.6.1.4.1.311.97.101.201.301.401"
RELEASE_GATE_PUBLIC_KEY = (
    "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C2964"
    "FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5"
)
RELEASE_GATE_NEXT_PUBLIC_KEY = (
    "7CF27B188D034F7E8A52380304B51AC3C08969E277F21B35A60B48FC476699780"
    "7775510DB8ED040293D9AC69F7430DBBA7DADE63CE982299E04B79D227873D1"
)
APPLICATION_SIGNER = "B2" * 32
INSTALLER_SIGNER = "C3" * 32
TAG_COMMIT = "1" * 40
BOOTSTRAP_TAG = {"SubLab808": "v1.4.0", "ReverseLab": "v1.1.0"}[PRODUCT]


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
        cls.release_state = (ROOT / "scripts" / "check-windows-release-state.py").read_text(
            encoding="utf-8"
        )
        cls.transition = (ROOT / "scripts" / "test-windows-updater-transition.ps1").read_text(
            encoding="utf-8"
        )
        cls.bootstrap_policy = json.loads(
            (ROOT / "Installer" / "Windows" / "bootstrap-policy.json").read_text(
                encoding="utf-8"
            )
        )
        cls.package_config = json.loads(
            (ROOT / "Installer" / "Windows" / "package-config.json").read_text(
                encoding="utf-8"
            )
        )
        cls.validator = load_module(
            "windows_release_validator", "validate-windows-release-assets.py"
        )
        cls.macos_validator = load_module("macos_release_assets", "macos-release-assets.py")
        cls.physical_receipt = load_module(
            "physical_daw_release_receipt",
            "validate-physical-daw-release-receipt.py",
        )

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

    def test_protection_source_and_owner_gates_precede_publication(self) -> None:
        self.assertEqual(self.release.count("scripts/validate-release-protection.py"), 3)
        authorization = self.release[:self.release.index("  build-windows:")]
        self.assertIn("scripts/validate-release-protection.py", authorization)
        stage = self.release[self.release.index("  stage-release:"):self.release.index("  accept-windows-upgrade:")]
        self.assertLess(stage.index("scripts/source-release.py verify"), stage.index("gh api --method POST"))
        self.assertIn("source-release]", stage)
        self.assertIn(f"release-assets/source/{PRODUCT}-$version-Source.zip", stage)
        physical = self.release[self.release.index("  physical-daw-acceptance:"):self.release.index("  finalize-release:")]
        self.assertIn('--evidence-directory "$gate_directory/evidence"', physical)
        finalizer = self.release[self.release.index("  finalize-release:"):]
        self.assertLess(finalizer.index("scripts/validate-owner-promotion.py"), finalizer.index("-F draft=false -F prerelease=false -f make_latest=true"))
        self.assertIn('quarantine \'explicit owner promotion approval is missing or invalid\'', finalizer)

    def test_release_state_authorization_is_immutable_and_fail_closed(self) -> None:
        for token in (
            "confirm_first_windows_release_bootstrap:",
            "WK_BOOTSTRAP_CONFIRMATION: ${{ inputs.confirm_first_windows_release_bootstrap }}",
            "authorize_windows_release:",
            "needs: authorize_windows_release",
            "needs.authorize_windows_release.result == 'success'",
            "scripts/check-windows-release-state.py",
            "ref: ${{ github.sha }}",
            'git show "$GITHUB_SHA:scripts/check-windows-release-state.py"',
            'git show "$GITHUB_SHA:Installer/Windows/bootstrap-policy.json"',
            "--policy-json",
            "--paginate --slurp",
            "--candidate-tag",
            "--confirmation",
            "IMMUTABLE_RELEASES_ADMIN_READ_TOKEN",
            "immutable-releases",
            "X-GitHub-Api-Version: 2026-03-10",
            ".enabled == true",
            "release_state_sha256",
            "baseline_release_id",
            "baseline_commit",
        ):
            self.assertIn(token, self.release)
        for token in (
            "Slurped release metadata contains a short intermediate page",
            "Release metadata contains duplicate IDs",
            "Windows release {tag} is not immutable",
            "Allowed candidate release is not the complete stable Windows candidate",
            "Candidate tag is not strictly newer than every stable or quarantined",
            "First Windows release bootstrap requires explicit confirmation",
        ):
            self.assertIn(token, self.release_state)
        self.assertGreaterEqual(self.release.count("needs: authorize_windows_release"), 2)
        self.assertNotIn("check-windows-release-bootstrap.py", self.release)
        self.assertNotIn("--paginated", self.release)
        self.assertEqual(
            self.bootstrap_policy,
            {"bootstrapTag": BOOTSTRAP_TAG, "product": PRODUCT, "schemaVersion": 1},
        )
        for token in ("Bootstrap", "N→N+1", "immutable", "Quarantäne"):
            self.assertIn(token, self.docs)

    @unittest.skipIf(shutil.which("jq") is None, "jq is required for workflow pagination")
    def test_candidate_filter_rechunks_every_pagination_boundary(self) -> None:
        rechunk_contract = re.compile(
            r"\[\.\[\]\[\] \| select\(\.id != \$id\)\] as \$history \|\s*"
            r"\[range\(0; \(\$history \| length\); 100\) as \$offset \|\s*"
            r"\$history\[\$offset:\(\$offset \+ 100\)\]\] \|\s*"
            r"if length == 0 then \[\[\]\] else \. end"
        )
        self.assertEqual(len(rechunk_contract.findall(self.release)), 2)
        program = (
            "[.[][] | select(.id != $id)] as $history | "
            "[range(0; ($history | length); 100) as $offset | "
            "$history[$offset:($offset + 100)]] | "
            "if length == 0 then [[]] else . end"
        )
        candidate_id = 9_999_999
        for history_count in (0, 1, 99, 100, 101, 199, 200, 201):
            with self.subTest(history_count=history_count):
                records = [{"id": candidate_id}] + [
                    {"id": release_id} for release_id in range(1, history_count + 1)
                ]
                pages = [records[offset : offset + 100] for offset in range(0, len(records), 100)]
                completed = subprocess.run(
                    ["jq", "--argjson", "id", str(candidate_id), program],
                    input=json.dumps(pages),
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                filtered = json.loads(completed.stdout)
                self.assertGreaterEqual(len(filtered), 1)
                self.assertTrue(all(len(page) == 100 for page in filtered[:-1]))
                self.assertLessEqual(len(filtered[-1]), 100)
                self.assertEqual(
                    [entry["id"] for page in filtered for entry in page],
                    list(range(1, history_count + 1)),
                )

    def test_dispatch_values_are_never_interpolated_into_shell_source(self) -> None:
        self.assertGreaterEqual(self.release.count("WK_INPUT_TAG: ${{ inputs.tag }}"), 7)
        self.assertEqual(self.release.count("$tag = $env:WK_INPUT_TAG"), 1)
        self.assertGreaterEqual(self.release.count('tag="$WK_INPUT_TAG"'), 5)
        self.assertEqual(
            self.release.count(
                "WK_DEFAULT_BRANCH: ${{ github.event.repository.default_branch }}"
            ),
            4,
        )
        self.assertEqual(self.release.count("$defaultBranch = $env:WK_DEFAULT_BRANCH"), 1)
        self.assertEqual(self.release.count('default_branch="$WK_DEFAULT_BRANCH"'), 3)
        allowed_prefixes = ("run-name:", "ref:", "url:", "WK_INPUT_TAG:")
        unsafe_lines = [
            line
            for line in self.release.splitlines()
            if "${{ inputs.tag }}" in line
            and not line.strip().startswith(allowed_prefixes)
        ]
        self.assertEqual(unsafe_lines, [])
        unsafe_default_branch_lines = [
            line
            for line in self.release.splitlines()
            if "${{ github.event.repository.default_branch }}" in line
            and not line.strip().startswith("WK_DEFAULT_BRANCH:")
        ]
        self.assertEqual(unsafe_default_branch_lines, [])

    def test_native_architectures_and_production_build_are_separate(self) -> None:
        for token in (
            "runner: windows-2022",
            "runner: windows-11-vs2026-arm",
            "platform: x64",
            "platform: ARM64EC",
            "artifact_arch: x64",
            "artifact_arch: arm64ec",
            f"-D{PRODUCT.upper()}_WINDOWS_UPDATER_PROFILE_EKU:STRING=$env:WK_PROFILE_EKU",
            f"-D{PRODUCT.upper()}_WINDOWS_UPDATER_NEXT_PROFILE_EKU:STRING=$env:WK_NEXT_PROFILE_EKU",
            f"{PRODUCT}_VST3 {PRODUCT}WindowsUpdater",
            f"Contents/Helpers/{PRODUCT}Updater.exe",
            "WindowsUpdaterPolicyTests",
            "WindowsUpdaterSelfTests",
            "HostTests",
            "PresetTests",
        ):
            self.assertIn(token, self.release)
        self.assertIn("runs-on: ${{ matrix.runner }}\n    timeout-minutes: 120", self.release)

    def test_oidc_signing_is_non_exportable_and_profile_pinned(self) -> None:
        for token in (
            "environment: release-signing", "id-token: write", "azure/login@",
            "vars.AZURE_CLIENT_ID", "vars.AZURE_TENANT_ID", "vars.AZURE_SUBSCRIPTION_ID",
            "vars.WINDOWS_ARTIFACT_SIGNING_PROFILE_EKU",
            "vars.WINDOWS_NEXT_ARTIFACT_SIGNING_PROFILE_EKU",
            "Test-ArtifactSigningProfileEku", "scripts/setup-artifact-signing.ps1",
            "http://timestamp.acs.microsoft.com/", "az account clear",
        ):
            self.assertIn(token, self.release)
        for forbidden in ("WINDOWS_CODE_SIGNING_PFX", "Import-PfxCertificate",
                          "WINDOWS_RELEASE_GATE_PRIVATE_KEY_PKCS8_BASE64"):
            self.assertNotIn(forbidden, self.release)
        setup = (ROOT / "scripts/setup-artifact-signing.ps1").read_text()
        for token in ("Get-FileHash", "SHA256", "SHA512", "ExcludeCredentials",
                      "EnvironmentCredential", "ManagedIdentityCredential",
                      "WK_SIGNING_DLIB_PATH", "DOTNET_ROOT_X64"):
            self.assertIn(token.lower(), setup.lower())

    def test_packager_receives_complete_signed_production_contract(self) -> None:
        for token in (
            "scripts/build-windows-installer.ps1",
            "UpdaterPath = $updater",
            "SourceCommit = $env:WK_TAG_COMMIT",
            "ExpectedProfileEku = $env:WK_PROFILE_EKU",
            "$buildArguments['ExpectedNextProfileEku'] = $env:WK_NEXT_PROFILE_EKU",
            "ExpectedReleaseGatePublicKeyXY = $env:WK_RELEASE_GATE_PUBLIC_KEY_XY",
            "$buildArguments['ExpectedReleaseGateNextPublicKeyXY'] =",
            "HostTestPath = $hostTest",
            "SignToolPath = $signTool",
            "SigningDlibPath = $env:WK_SIGNING_DLIB_PATH",
            "SigningMetadataPath = $env:WK_SIGNING_METADATA_PATH",
            "TimestampUrl = $env:TIMESTAMP_URL",
            "scripts/validate-windows-release-assets.py",
            "'--source-commit', $env:WK_TAG_COMMIT",
            "@('--expected-next-profile-eku', $env:WK_NEXT_PROFILE_EKU)",
            "'--expected-release-gate-public-key-xy', $env:WK_RELEASE_GATE_PUBLIC_KEY_XY",
            "'--expected-release-gate-next-public-key-xy'",
        ):
            self.assertIn(token, self.release)
        self.assertNotIn("-AllowUnsigned", self.release)

    def test_empty_next_pin_is_omitted_from_native_argument_lists(self) -> None:
        guard = "if (-not [string]::IsNullOrEmpty($env:WK_NEXT_PROFILE_EKU))"
        self.assertEqual(self.release.count(guard), 3)
        release_gate_guard = (
            "if (-not [string]::IsNullOrEmpty($env:WK_RELEASE_GATE_NEXT_PUBLIC_KEY_XY))"
        )
        self.assertEqual(self.release.count(release_gate_guard), 3)
        for token in (
            "& ./scripts/build-windows-installer.ps1 @buildArguments",
            "& python @validatorArguments",
            "& ./scripts/test-windows-installer.ps1 @acceptanceArguments",
        ):
            self.assertIn(token, self.release)
        self.assertNotIn("-ExpectedNextProfileEku $env:WK_NEXT_PROFILE_EKU", self.release)
        self.assertNotIn("--expected-next-profile-eku $env:WK_NEXT_PROFILE_EKU", self.release)
        self.assertNotIn(
            "-ExpectedReleaseGateNextPublicKeyXY $env:WK_RELEASE_GATE_NEXT_PUBLIC_KEY_XY",
            self.release,
        )

    def test_signed_msi_is_installed_loaded_and_removed_before_upload(self) -> None:
        for token in (
            "Install, load and uninstall signed Windows MSI",
            "scripts/test-windows-installer.ps1",
            "MsiPath = $msi",
            "EvidencePath = $evidence",
            "HostTestPath = $hostTest",
            f"Product = '{PRODUCT}'",
            "Architecture = '${{ matrix.artifact_arch }}'",
            "ExpectedMsiArchitecture = '${{ matrix.msi_architecture }}'",
            "ExpectedVersion = $env:WK_RELEASE_VERSION",
            "ExpectedManufacturer = 'Whykiki Audio'",
            "ExpectedUpgradeCode = '${{ matrix.upgrade_code }}'",
            "ExpectedOtherArchitectureUpgradeCode = '${{ matrix.other_architecture_upgrade_code }}'",
            "ExpectedProfileEku = $env:WK_PROFILE_EKU",
            "$acceptanceArguments['ExpectedNextProfileEku'] = $env:WK_NEXT_PROFILE_EKU",
            "ExpectedReleaseGatePublicKeyXY = $env:WK_RELEASE_GATE_PUBLIC_KEY_XY",
            "$acceptanceArguments['ExpectedReleaseGateNextPublicKeyXY'] =",
        ):
            self.assertIn(token, self.release)
        validator = self.release.index("scripts/validate-windows-release-assets.py")
        acceptance = self.release.index("scripts/test-windows-installer.ps1")
        upload = self.release.index("- name: Upload signed release candidate")
        self.assertLess(validator, acceptance)
        self.assertLess(acceptance, upload)
        self.assertIn("msi_architecture: x64", self.release)
        self.assertIn("msi_architecture: arm64", self.release)
        for architecture, msi_architecture in (("x64", "x64"), ("arm64ec", "arm64")):
            other = "arm64ec" if architecture == "x64" else "x64"
            self.assertRegex(
                self.release,
                rf"- artifact_arch: {architecture}\n"
                rf"\s+msi_architecture: {msi_architecture}\n"
                rf"\s+upgrade_code: {self.package_config['upgradeCodes'][architecture]}\n"
                rf"\s+other_architecture_upgrade_code: {self.package_config['upgradeCodes'][other]}\n",
            )
        for limitation in ("N→N+1", "Downgrade", "x64↔ARM64EC", "separate physische Abnahme"):
            self.assertIn(limitation, self.docs)

    def test_release_is_staged_as_the_exact_immutable_public_prerelease(self) -> None:
        self.assertEqual(self.release.count("contents: write"), 2)
        start = self.release.index("  stage-release:")
        end = self.release.index("\n  accept-windows-upgrade:", start)
        stage = self.release[start:end]
        for token in (
            "needs: [authorize_windows_release, build-windows, build-macos, test-macos-intel-candidate, source-release]",
            "actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093",
            "merge-multiple: true",
            "github.run_attempt",
            "--architecture x64 --architecture arm64ec",
            '--source-commit "$tag_commit"',
            "SHA256SUMS.txt",
            "cleanup_created_draft()",
            "publication_attempted=false",
            "publication_attempted=true",
            "verify_release_assets()",
            "candidate_manifest_sha256()",
            "resolve_public_history",
            "read_latest_state()",
            "verify_origin_tag()",
            "gh api graphql",
            "gh api --method POST -H 'X-GitHub-Api-Version: 2026-03-10'",
            '-f tag_name="$tag" -f target_commitish="$tag_commit"',
            '-F draft=true -F prerelease=false',
            'created_release_id="$(jq -r \'.id\' "$create_response")"',
            "https://uploads.github.com/repos/$GITHUB_REPOSITORY/releases/$created_release_id/assets?name=$name",
            "--data-binary \"@$asset\"",
            "-F draft=false -F prerelease=true -f make_latest=false",
            "verify_candidate_identity \"$candidate_metadata\" false true true",
            ".immutable == true",
            "staged_verified=true",
            "Latest is unchanged",
        ):
            self.assertIn(token, stage)
        for architecture in ("x64", "arm64ec"):
            self.assertIn(
                f'release-assets/windows/{PRODUCT}-$version-Windows-{architecture}.msi', stage
            )
            self.assertIn(
                f'release-assets/windows/{PRODUCT}-$version-Windows-{architecture}.evidence.json',
                stage,
            )
        steps_start = stage.index("\n    steps:")
        self.assertNotIn("GH_TOKEN", stage[:steps_start])
        revalidate_start = stage.index(
            "- name: Revalidate source, evidence, signatures, notarization and all hashes"
        )
        policy_start = stage.index(
            "- name: Require immutable GitHub Releases policy immediately before staging"
        )
        self.assertNotIn("GH_TOKEN", stage[revalidate_start:policy_start])
        publish_start = stage.index(
            "- name: Create a complete draft and expose that exact ID as an immutable prerelease"
        )
        publish_run = stage.index("        run: |", publish_start)
        self.assertIn(
            "env:\n          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}",
            stage[publish_start:publish_run],
        )
        self.assertNotIn("gh release upload", stage)
        cleanup_start = stage.index("cleanup_created_draft()")
        cleanup_end = stage.index("trap cleanup_created_draft EXIT", cleanup_start)
        cleanup = stage[cleanup_start:cleanup_end]
        self.assertIn('if [[ "$publication_attempted" == true ]]', cleanup)
        self.assertIn("It is never deleted automatically", cleanup)
        self.assertIn("gh api --method DELETE", cleanup)
        self.assertLess(
            cleanup.index('if [[ "$publication_attempted" == true ]]'),
            cleanup.index("gh api --method DELETE"),
        )

    def test_release_gate_key_is_preflighted_and_revalidated_before_staging(self) -> None:
        build_start = self.release.index("  build-windows:")
        build_end = self.release.index("\n  build-macos:", build_start)
        build = self.release[build_start:build_end]

        key_preflight_start = build.index(
            "- name: Verify non-exportable release-gate public key"
        )
        key_preflight_end = build.index(
            "- name: Prove the installed baseline authorizes the active release-gate key",
            key_preflight_start,
        )
        key_preflight = build[key_preflight_start:key_preflight_end]
        for token in (
            "scripts/verify-release-gate-key.ps1",
            "-KeyId $env:RELEASE_GATE_KEY_VAULT_KEY_ID",
            "-ExpectedPublicKeyXY $env:WK_RELEASE_GATE_PUBLIC_KEY_XY",
        ):
            self.assertIn(token, key_preflight)
        self.assertNotIn("PRIVATE_KEY", key_preflight)
        self.assertLess(
            key_preflight_start,
            build.index("- name: Configure production updater pin"),
        )

        baseline_start = key_preflight_end
        baseline_end = build.index("- name: Configure production updater pin", baseline_start)
        baseline = build[baseline_start:baseline_end]
        for token in (
            "if: needs.authorize_windows_release.outputs.mode == 'upgrade'",
            '"repos/$env:GITHUB_REPOSITORY/releases/$env:WK_BASELINE_RELEASE_ID"',
            "$release.immutable -isnot [bool] -or -not $release.immutable",
            "@($release.assets).Count -ne 9",
            'Windows-${{ matrix.artifact_arch }}.evidence.json',
            "$evidenceAssets.Count -ne 1",
            "$asset.digest -cnotmatch '^sha256:[0-9a-f]{64}$'",
            "$item.Length -ne [long]$asset.size",
            '"sha256:$actualDigest" -cne [string]$asset.digest',
            "$evidence.schemaVersion -ne 5",
            "$evidence.releaseGatePublicKeyXY",
            "$evidence.releaseGateNextPublicKeyXY",
            "$evidence.releaseGatePublicKeyAllowlistXY",
            "$allowlist[$index] -cne $expectedAllowlist[$index]",
            "$allowlist -cnotcontains $env:WK_RELEASE_GATE_PUBLIC_KEY_XY",
        ):
            self.assertIn(token, baseline)

        stage_start = self.release.index("  stage-release:")
        stage_end = self.release.index("\n  accept-windows-upgrade:", stage_start)
        stage = self.release[stage_start:stage_end]
        self.assertIn(
            "needs: [authorize_windows_release, build-windows, build-macos, test-macos-intel-candidate, source-release]",
            stage,
        )
        for token in (
            'release_gate_key="$RELEASE_GATE_PUBLIC_KEY_XY"',
            'next_release_gate_key="$RELEASE_GATE_NEXT_PUBLIC_KEY_XY"',
            '[[ "$release_gate_key" =~ ^[0-9A-F]{128}$ ]]',
            "python3 scripts/validate-windows-release-assets.py",
            '--expected-release-gate-public-key-xy "$release_gate_key"',
            '--expected-release-gate-next-public-key-xy "$next_release_gate_key"',
            "--architecture x64 --architecture arm64ec",
        ):
            self.assertIn(token, stage)
        self.assertNotIn("WINDOWS_RELEASE_GATE_PRIVATE_KEY_PKCS8_BASE64", stage)

    def test_native_transition_gate_binds_public_n_to_exact_candidate(self) -> None:
        start = self.release.index("  accept-windows-upgrade:")
        end = self.release.index("\n  physical-daw-acceptance:", start)
        acceptance = self.release[start:end]
        for token in (
            "runner: windows-2022",
            "runner: windows-11-vs2026-arm",
            "needs: [authorize_windows_release, stage-release]",
            "needs.stage-release.outputs.staged_verified == 'true'",
            'git show "${env:GITHUB_SHA}:scripts/test-windows-updater-transition.ps1"',
            'git show "${env:GITHUB_SHA}:scripts/test-windows-installer.ps1"',
            "Mode = $env:WK_TRANSITION_MODE",
            "BaselineReleaseId = $baselineReleaseId",
            "CandidateReleaseId = [long]$env:WK_CANDIDATE_RELEASE_ID",
            "CandidateSourceCommit = $env:WK_CANDIDATE_COMMIT",
            "HostTestPath =",
            "Upload transition acceptance receipt",
            "retention-days: 30",
        ):
            self.assertIn(token, acceptance)
        self.assertIn("permissions:\n      contents: read", acceptance)
        self.assertIn("id-token: write", acceptance)
        self.assertIn("ReleaseGateKeyVaultKeyId = $env:RELEASE_GATE_KEY_VAULT_KEY_ID", acceptance)
        self.assertNotIn("PRIVATE_KEY_PKCS8", acceptance)
        self.assertNotIn("secrets.GITHUB_TOKEN", acceptance)

        for token in (
            "$script:Owner = 'TheWhykiki'",
            "X-GitHub-Api-Version' = '2026-03-10'",
            "$Release.immutable",
            "Release does not contain exactly nine cross-platform assets.",
            "256MB",
            "8MB",
            "-TimeoutSec 600",
            "ExpectedPrerelease",
            "'bootstrap'",
            "'upgrade'",
            "Installed baseline updater",
            "ExpectedExecutableSha256",
            "GetNamedPipeClientProcessId",
            "ParentProcessId",
            "PipeOptions]::CurrentUserOnly",
            "[byte[]]::new($expectedBytes.Length + 1)",
            "operationMode",
            "releaseId",
            "releaseTag",
            "sourceCommit",
            "phase -ceq 'verified'",
            "Older baseline MSI was not rejected as a downgrade.",
            "Updater-installed candidate host load",
            "Test-owned updater operation cleanup failed.",
            "productCleanup = $true",
            "[int] $UpdaterTimeoutSeconds = 3600",
            "$ExpiresAtUnixSeconds -le $nowUnixSeconds + 300",
            "$AuthorizationSignatureP1363 -cmatch '^[0-9A-F]{128}$'",
        ):
            self.assertIn(token, self.transition)
        cli = """'--release-gate',
            '--challenge', $Challenge,
            '--response-pipe', $PipeName,
            '--parent-process-id', ([string]$PID),
            '--release-id', ([string]$CandidateReleaseId),
            '--tag', $CandidateTag,
            '--source-commit', $CandidateSourceCommit,
            '--parent-process-created-at-filetime', ([string]$ParentProcessCreatedAtFiletime),
            '--expires-at-unix-seconds', ([string]$ExpiresAtUnixSeconds),
            '--authorization-signature-p1363', $AuthorizationSignatureP1363"""
        self.assertIn(cli, self.transition)

        authorization_start = self.transition.index("function New-ReleaseGateAuthorization")
        authorization_end = self.transition.index(
            "function Assert-NamedPipeReleaseGateClient", authorization_start
        )
        authorization = self.transition[authorization_start:authorization_end]
        canonical_fields = (
            "domain=whykiki.windows-updater-release-gate-authorization",
            "schemaVersion=1",
            "repository=",
            "product=",
            "architecture=",
            "installedVersion=",
            "releaseId=",
            "tag=",
            "sourceCommit=",
            "challenge=",
            "responsePipe=",
            "parentProcessId=",
            "parentProcessCreatedAtFiletime=",
            "expiresAtUnixSeconds=",
        )
        positions = [authorization.index(field) for field in canonical_fields]
        self.assertEqual(positions, sorted(positions))
        self.assertIn(
            "[DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 240", authorization
        )
        self.assertIn("New-LowSReleaseGateSignature $SigningKey $messageBytes", authorization)

        invoke_start = self.transition.index("function Invoke-ReleaseGateUpdater")
        invoke_end = self.transition.index(
            "function Remove-VerifiedOperationDirectories", invoke_start
        )
        invoke = self.transition[invoke_start:invoke_end]
        self.assertIn("$startInfo.Environment.Remove('AZURE_CLIENT_SECRET')", invoke)
        self.assertNotIn("ReleaseGatePrivateKeyPkcs8Base64", invoke)
        key_helper = (ROOT / "scripts/release-gate-key.ps1").read_text()
        for token in ("keyvault key show", "keyvault key sign", "EC-HSM",
                      "ES256", "exportable", "New-LowSReleaseGateSignature",
                      "VerifyData", "IeeeP1363FixedFieldConcatenation"):
            self.assertIn(token, key_helper)
        self.assertNotIn("ImportPkcs8PrivateKey", key_helper)

        journal_start = self.transition.index("function Remove-VerifiedOperationDirectories")
        journal_end = self.transition.index("if (-not $IsWindows)", journal_start)
        journal = self.transition[journal_start:journal_end]
        for forbidden_ephemeral_value in (
            "$Challenge",
            "$PipeName",
            "$AuthorizationSignatureP1363",
            "authorizationSignature|expiresAtUnixSeconds|parentProcessCreatedAtFiletime",
            "private.?key|pkcs8|WINDOWS_RELEASE_GATE_PRIVATE_KEY",
        ):
            self.assertIn(forbidden_ephemeral_value, journal)
        self.assertNotIn("ReadToEndAsync", self.transition)
        self.assertNotIn("GH_TOKEN", self.transition)

    def test_finalizer_reconciles_uncertain_promotion_and_never_rewrites_assets(self) -> None:
        start = self.release.index("  finalize-release:")
        finalizer = self.release[start:]
        for token in (
            "always()",
            "needs: [authorize_windows_release, stage-release, accept-windows-upgrade, physical-daw-acceptance]",
            "WK_ACCEPTANCE_RESULT: ${{ needs.accept-windows-upgrade.result }}",
            "WK_PHYSICAL_ACCEPTANCE_RESULT: ${{ needs.physical-daw-acceptance.result }}",
            "WK_PHYSICAL_RECEIPT_SHA256: ${{ needs.physical-daw-acceptance.outputs.receipt_sha256 }}",
            "WK_PHYSICAL_RECEIPT_BASE64: ${{ needs.physical-daw-acceptance.outputs.receipt_base64 }}",
            "physicalDawGate=$WK_PHYSICAL_ACCEPTANCE_RESULT",
            "whykiki-physical-daw-receipt-v2",
            "WK_ASSET_MANIFEST_SHA256",
            "fail_unknown()",
            "quarantine()",
            "fail_after_promotion()",
            "promotion_request_succeeded=false",
            "if gh api --method PATCH -H 'X-GitHub-Api-Version: 2026-03-10'",
            '-F draft=false -F prerelease=false -f make_latest=true',
            "for attempt in 1 2 3 4 5",
            ".prerelease == false and .immutable == true",
            ".prerelease == true and .immutable == true",
            "promotion outcome is UNKNOWN",
            "--allow-candidate-release-id \"$release_id\"",
            '[[ "$final_state" == "$WK_INITIAL_RELEASE_STATE" ]]',
            "post-promotion history attestation failed",
            f'-f name="{PRODUCT} ${{tag#v}}" -f body="$stable_body"',
        ):
            self.assertIn(token, finalizer)
        patch = finalizer.index("if gh api --method PATCH")
        reconcile = finalizer.index("for attempt in 1 2 3 4 5", patch)
        stable = finalizer.index(".prerelease == false and .immutable == true", reconcile)
        quarantine = finalizer.index(".prerelease == true and .immutable == true", stable)
        unknown = finalizer.index("promotion outcome is UNKNOWN", quarantine)
        post_state = finalizer.index("--allow-candidate-release-id", unknown)
        self.assertLess(patch, reconcile)
        self.assertLess(reconcile, stable)
        self.assertLess(stable, quarantine)
        self.assertLess(quarantine, unknown)
        self.assertLess(unknown, post_state)
        self.assertNotIn("--method DELETE", finalizer)

    def test_physical_daw_gate_is_protected_exact_and_fail_closed(self) -> None:
        start = self.release.index("  physical-daw-acceptance:")
        end = self.release.index("\n  finalize-release:", start)
        gate = self.release[start:end]
        for token in (
            "needs: [authorize_windows_release, stage-release, accept-windows-upgrade]",
            "needs.accept-windows-upgrade.result == 'success'",
            "name: physical-daw-release",
            "actions: read",
            "contents: read",
            "validate-physical-daw-release-receipt.py",
            '"repos/$GITHUB_REPOSITORY/environments/$WK_PHYSICAL_ENVIRONMENT"',
            '"repos/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID/approvals"',
            '"repos/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID"',
            '"repos/$GITHUB_REPOSITORY" > "$gate_directory/repository.json"',
            '"repos/$GITHUB_REPOSITORY/branches/$default_branch_uri"',
            '"repos/$GITHUB_REPOSITORY/compare/$WK_CANDIDATE_COMMIT...$observed_branch_commit"',
            '"repos/$GITHUB_REPOSITORY/compare/$WK_WORKFLOW_SHA...$observed_branch_commit"',
            '"repos/$GITHUB_REPOSITORY/releases/$WK_CANDIDATE_RELEASE_ID"',
            "--branch-json \"$gate_directory/branch.json\"",
            "--repository-json \"$gate_directory/repository.json\"",
            "--candidate-compare-json \"$gate_directory/candidate-compare.json\"",
            "--workflow-compare-json \"$gate_directory/workflow-compare.json\"",
            "--run-attempt \"$GITHUB_RUN_ATTEMPT\"",
            "--asset-manifest-sha256 \"$WK_ASSET_MANIFEST_SHA256\"",
            "Upload validated physical DAW receipt",
            "retention-days: 90",
        ):
            self.assertIn(token, gate)
        self.assertNotIn("WINDOWS_CODE_SIGNING_PFX_BASE64", gate)
        self.assertNotIn("'.repository.default_branch |", gate)
        self.assertNotIn("MACOS_NOTARY_PRIVATE_KEY_P8_BASE64", gate)
        self.assertNotIn("WINDOWS_RELEASE_GATE_PRIVATE_KEY_PKCS8_BASE64", gate)
        finalizer = self.release[end:]
        receipt_check = finalizer.index('"$WK_PHYSICAL_ACCEPTANCE_RESULT" != success')
        promotion = finalizer.index("if gh api --method PATCH")
        self.assertLess(receipt_check, promotion)
        self.assertIn("physical DAW receipt digest changed after environment approval", finalizer)
        for token in (
            "WK_PHYSICAL_REVIEWER_ID",
            '"repos/$GITHUB_REPOSITORY/branches/$current_default_branch_uri"',
            "verify_current_ancestor \"$WK_CANDIDATE_COMMIT\" 'candidate commit'",
            "verify_current_ancestor \"$GITHUB_SHA\" 'workflow commit'",
        ):
            self.assertIn(token, finalizer)
        self.assertLess(
            finalizer.index("verify_current_ancestor \"$GITHUB_SHA\""),
            promotion,
        )

    def _physical_receipt_fixture(self) -> dict[str, object]:
        version = "1.2.3"
        tag = f"v{version}"
        run_id = 123456789
        run_attempt = 1
        release_id = 987654321
        workflow_sha = "2" * 40
        environment_id = 41
        reviewer_id = 12602174
        names = (
            f"{PRODUCT}-{version}-Windows-x64.msi",
            f"{PRODUCT}-{version}-Windows-x64.evidence.json",
            f"{PRODUCT}-{version}-Windows-arm64ec.msi",
            f"{PRODUCT}-{version}-Windows-arm64ec.evidence.json",
            f"{PRODUCT}-{version}-macOS-universal.pkg",
            f"{PRODUCT}-{version}-macOS-universal-VST3.zip",
            f"{PRODUCT}-{version}-macOS-universal.evidence.json",
            f"{PRODUCT}-{version}-Source.zip",
            f"{PRODUCT}-{version}-SHA256SUMS.txt",
        )
        assets = [
            {
                "digest": f"sha256:{index:064x}",
                "id": 1000 + index,
                "name": name,
                "size": 2000 + index,
                "state": "uploaded",
            }
            for index, name in enumerate(names, start=1)
        ]
        by_name = {asset["name"]: asset for asset in assets}
        binary_hashes = {"windows-x64-msi": "a" * 64, "windows-arm64ec-msi": "b" * 64,
                         "macos-universal-pkg": "c" * 64, "macos-universal-zip": "c" * 64}
        evidence_files = {}
        for architecture, folder in (("x64", "x86_64-win"), ("arm64ec", "arm64ec-win")):
            evidence = {
                "product": PRODUCT, "version": version, "sourceCommit": TAG_COMMIT,
                "artifactStatus": "SIGNED",
                "msiSha256": by_name[f"{PRODUCT}-{version}-Windows-{architecture}.msi"]["digest"][7:],
                "payloadFiles": [{"path": f"Contents\\{folder}\\{PRODUCT}.vst3",
                                  "sha256": binary_hashes[f"windows-{architecture}-msi"]}],
            }
            evidence_files[f"{PRODUCT}-{version}-Windows-{architecture}.evidence.json"] = json.dumps(evidence).encode()
        evidence_files[f"{PRODUCT}-{version}-macOS-universal.evidence.json"] = json.dumps({
            "product": PRODUCT, "version": version, "commit": TAG_COMMIT,
            "artifactStatus": "SIGNED-NOTARIZED",
            "packageSha256": by_name[f"{PRODUCT}-{version}-macOS-universal.pkg"]["digest"][7:],
            "vst3ZipSha256": by_name[f"{PRODUCT}-{version}-macOS-universal-VST3.zip"]["digest"][7:],
            "vst3BinarySha256": binary_hashes["macos-universal-pkg"],
        }).encode()
        for name, encoded in evidence_files.items():
            by_name[name]["digest"] = "sha256:" + hashlib.sha256(encoded).hexdigest()
            by_name[name]["size"] = len(encoded)
        manifest_sha256 = self.physical_receipt._manifest_sha256(assets)
        receipt = {
            "schemaVersion": 2,
            "repository": f"TheWhykiki/{PRODUCT}",
            "product": PRODUCT,
            "runId": run_id,
            "runAttempt": run_attempt,
            "releaseId": release_id,
            "tag": tag,
            "commit": TAG_COMMIT,
            "assetManifestSha256": manifest_sha256,
            "artifacts": {
                name: next(asset["digest"] for asset in assets if asset["name"] == name)
                for name in (
                    f"{PRODUCT}-{version}-Windows-x64.msi",
                    f"{PRODUCT}-{version}-Windows-arm64ec.msi",
                    f"{PRODUCT}-{version}-macOS-universal.pkg",
                    f"{PRODUCT}-{version}-macOS-universal-VST3.zip",
                )
            },
            "checks": [
                {
                    "platform": platform,
                    "cpuArchitecture": architecture,
                    "hostProcessArchitecture": "arm64ec" if platform == "windows-arm64ec-msi" else architecture,
                    "host": host,
                    "hostVersion": "13.0.50" if host == "Cubase" else "7.50",
                    "osVersion": "Windows 11 24H2" if platform.startswith("windows") else "macOS 15.6",
                    "machine": f"qa-{platform}-{architecture}",
                    "tester": "qa-operator",
                    "testedAt": "2026-09-08T12:00:00Z",
                    "result": "pass",
                    "loadedVst3Path": (
                        f"C:\\Program Files\\Common Files\\VST3\\{PRODUCT}.vst3\\Contents\\"
                        + ("arm64ec-win" if architecture == "arm64" else "x86_64-win")
                        + f"\\{PRODUCT}.vst3" if platform.startswith("windows")
                        else f"/Library/Audio/Plug-Ins/VST3/{PRODUCT}.vst3/Contents/MacOS/{PRODUCT}"),
                    "loadedVst3Sha256": binary_hashes[platform],
                }
                for platform, architectures in (
                    ("windows-x64-msi", ("x86_64",)),
                    ("windows-arm64ec-msi", ("arm64",)),
                    ("macos-universal-pkg", ("x86_64", "arm64")),
                    ("macos-universal-zip", ("x86_64", "arm64")),
                )
                for architecture in architectures
                for host in ("Cubase", "Reaper")
            ],
        }
        environment = {
            "id": environment_id,
            "name": "physical-daw-release",
            "can_admins_bypass": False,
            "deployment_branch_policy": {
                "protected_branches": True,
                "custom_branch_policies": False,
            },
            "protection_rules": [
                {
                    "type": "required_reviewers",
                    "prevent_self_review": False,
                    "reviewers": [
                        {
                            "type": "User",
                            "reviewer": {"id": reviewer_id, "login": "TheWhykiki", "type": "User"},
                        }
                    ],
                }
            ],
        }
        reviews = [
            {
                "state": "approved",
                "comment": json.dumps(receipt),
                "environments": [
                    {"id": environment_id, "name": "physical-daw-release"}
                ],
                "user": {"id": reviewer_id, "login": "TheWhykiki", "type": "User"},
            }
        ]
        workflow_run = {
            "id": run_id,
            "run_attempt": run_attempt,
            "event": "workflow_dispatch",
            "head_sha": workflow_sha,
            "head_branch": "main",
            "repository": {
                "id": 101,
                "full_name": f"TheWhykiki/{PRODUCT}",
                "owner": {"id": reviewer_id, "login": "TheWhykiki", "type": "User"},
            },
            "actor": {"id": 7, "login": "release-operator"},
            "triggering_actor": {"id": 8, "login": "rerun-operator"},
        }
        branch = {
            "name": "main",
            "protected": True,
            "commit": {"sha": workflow_sha},
        }
        candidate_compare = {
            "status": "ahead",
            "base_commit": {"sha": TAG_COMMIT},
            "merge_base_commit": {"sha": TAG_COMMIT},
        }
        workflow_compare = {
            "status": "identical",
            "base_commit": {"sha": workflow_sha},
            "merge_base_commit": {"sha": workflow_sha},
        }
        release = {
            "id": release_id,
            "tag_name": tag,
            "target_commitish": TAG_COMMIT,
            "draft": False,
            "prerelease": True,
            "immutable": True,
            "body": (
                f"whykiki-release-run:TheWhykiki/{PRODUCT}:"
                f"{run_id}:{run_attempt}:{TAG_COMMIT}"
            ),
            "published_at": "2026-09-08T11:00:00Z",
            "assets": assets,
        }
        return {
            "environment": environment,
            "reviews": reviews,
            "workflow_run": workflow_run,
            "repository_metadata": {
                "id": 101,
                "full_name": f"TheWhykiki/{PRODUCT}",
                "default_branch": "main",
                "owner": {"id": reviewer_id, "login": "TheWhykiki", "type": "User"},
            },
            "branch": branch,
            "candidate_compare": candidate_compare,
            "workflow_compare": workflow_compare,
            "release": release,
            "repository": f"TheWhykiki/{PRODUCT}",
            "product": PRODUCT,
            "environment_name": "physical-daw-release",
            "run_id": run_id,
            "run_attempt": run_attempt,
            "workflow_sha": workflow_sha,
            "release_id": release_id,
            "tag": tag,
            "commit": TAG_COMMIT,
            "asset_manifest_sha256": manifest_sha256,
            "evidence_files": evidence_files,
            "now": dt.datetime(2026, 9, 8, 13, 0, tzinfo=dt.timezone.utc),
        }

    def test_physical_receipt_binds_minimal_run_to_full_repository_metadata(self) -> None:
        fixture = self._physical_receipt_fixture()
        self.assertNotIn("default_branch", fixture["workflow_run"]["repository"])
        envelope = self.physical_receipt.validate(**fixture)
        self.assertEqual(envelope["branchProtection"]["name"], "main")
        # GitHub's minimal Actions repository omits this field; a null value
        # must not override the canonical full repository API response either.
        fixture["workflow_run"]["repository"]["default_branch"] = None
        self.assertEqual(self.physical_receipt.validate(**fixture), envelope)

        for mutation in (
            "missing-metadata", "metadata-full-name", "metadata-missing-full-name",
            "metadata-id", "metadata-missing-id", "metadata-bool-id", "run-missing-id",
            "metadata-missing-owner", "metadata-owner-id", "metadata-owner-login",
            "metadata-owner-type", "metadata-owner-missing-type",
            "metadata-owner-missing-login", "metadata-owner-bool-id",
            "missing-default-branch", "null-default-branch", "empty-default-branch",
            "wrong-default-branch", "control-default-branch", "long-default-branch",
        ):
            with self.subTest(mutation=mutation):
                fixture = self._physical_receipt_fixture()
                metadata = fixture["repository_metadata"]
                if mutation == "missing-metadata":
                    fixture["repository_metadata"] = None
                elif mutation == "metadata-full-name":
                    metadata["full_name"] = f"different-owner/{PRODUCT}"
                elif mutation == "metadata-missing-full-name":
                    del metadata["full_name"]
                elif mutation == "metadata-id":
                    metadata["id"] = 102
                elif mutation == "metadata-missing-id":
                    del metadata["id"]
                elif mutation == "metadata-bool-id":
                    metadata["id"] = True
                elif mutation == "run-missing-id":
                    del fixture["workflow_run"]["repository"]["id"]
                elif mutation == "metadata-missing-owner":
                    del metadata["owner"]
                elif mutation == "metadata-owner-id":
                    metadata["owner"]["id"] = 77
                elif mutation == "metadata-owner-login":
                    metadata["owner"]["login"] = "different-owner"
                elif mutation == "metadata-owner-type":
                    metadata["owner"]["type"] = "Organization"
                elif mutation == "metadata-owner-missing-type":
                    del metadata["owner"]["type"]
                elif mutation == "metadata-owner-missing-login":
                    del metadata["owner"]["login"]
                elif mutation == "metadata-owner-bool-id":
                    metadata["owner"]["id"] = True
                elif mutation == "missing-default-branch":
                    del metadata["default_branch"]
                elif mutation == "null-default-branch":
                    metadata["default_branch"] = None
                elif mutation == "empty-default-branch":
                    metadata["default_branch"] = ""
                elif mutation == "wrong-default-branch":
                    metadata["default_branch"] = "different-branch"
                elif mutation == "control-default-branch":
                    metadata["default_branch"] = "main\n"
                else:
                    metadata["default_branch"] = "a" * 256
                with self.assertRaises(self.physical_receipt.ContractError):
                    self.physical_receipt.validate(**fixture)

    def test_physical_receipt_owner_can_approve_own_run_and_rerun(self) -> None:
        for actors in (("actor",), ("triggering_actor",), ("actor", "triggering_actor")):
            with self.subTest(actors=actors):
                fixture = self._physical_receipt_fixture()
                owner = fixture["workflow_run"]["repository"]["owner"]
                for field in actors:
                    fixture["workflow_run"][field] = dict(owner)
                envelope = self.physical_receipt.validate(**fixture)
                self.assertEqual(envelope["review"]["userId"], owner["id"])
                self.assertEqual(len(envelope["receipt"]["checks"]), 12)

        fixture = self._physical_receipt_fixture()
        fixture["run_attempt"] = fixture["workflow_run"]["run_attempt"] = 2
        fixture["workflow_run"]["triggering_actor"] = dict(fixture["workflow_run"]["repository"]["owner"])
        receipt = json.loads(fixture["reviews"][0]["comment"])
        receipt["runAttempt"] = 2
        fixture["reviews"][0]["comment"] = json.dumps(receipt)
        fixture["release"]["body"] = (
            f"whykiki-release-run:TheWhykiki/{PRODUCT}:"
            f'{fixture["run_id"]}:2:{TAG_COMMIT}'
        )
        envelope = self.physical_receipt.validate(**fixture)
        self.assertEqual(envelope["receipt"]["runAttempt"], 2)
        self.assertEqual(envelope["review"]["userId"], 12602174)
        # Owner authority must not make a previous attempt's receipt reusable.
        receipt["runAttempt"] = 1
        fixture["reviews"][0]["comment"] = json.dumps(receipt)
        with self.assertRaises(self.physical_receipt.ContractError):
            self.physical_receipt.validate(**fixture)

    def test_physical_receipt_requires_exact_owner_identity_and_policy(self) -> None:
        for mutation in (
            "missing-owner", "owner-organization", "owner-bot", "owner-missing-type",
            "owner-id", "owner-bool-id", "owner-login", "owner-missing-login",
            "repository-owner-mismatch", "reviewer-id", "reviewer-login",
            "reviewer-bot", "reviewer-missing-type", "configured-id", "configured-login",
            "configured-team", "configured-bot", "configured-missing-type",
            "missing-reviewer", "additional-reviewer", "duplicate-owner-reviewer",
            "prevent-self-review", "missing-self-review-policy", "numeric-self-review-policy",
            "actor-owner-id-only", "actor-owner-login-only",
            "rerun-owner-id-only", "rerun-owner-login-only", "missing-actor",
            "missing-rerun-actor", "invalid-actor-id", "invalid-rerun-login",
            "actor-pair-inconsistent",
        ):
            with self.subTest(mutation=mutation):
                fixture = self._physical_receipt_fixture()
                run = fixture["workflow_run"]
                owner = run["repository"]["owner"]
                review_user = fixture["reviews"][0]["user"]
                rule = fixture["environment"]["protection_rules"][0]
                configured = rule["reviewers"][0]
                configured_user = configured["reviewer"]
                if mutation == "missing-owner":
                    del run["repository"]["owner"]
                elif mutation in ("owner-organization", "owner-bot"):
                    owner["type"] = "Organization" if mutation.endswith("organization") else "Bot"
                elif mutation == "owner-missing-type":
                    del owner["type"]
                elif mutation in ("owner-id", "owner-bool-id"):
                    owner["id"] = 77 if mutation == "owner-id" else True
                elif mutation == "owner-login":
                    owner["login"] = "different-owner"
                elif mutation == "owner-missing-login":
                    del owner["login"]
                elif mutation == "repository-owner-mismatch":
                    run["repository"]["full_name"] = f"different-owner/{PRODUCT}"
                elif mutation == "reviewer-id":
                    review_user["id"] = 77
                elif mutation == "reviewer-login":
                    review_user["login"] = "different-owner"
                elif mutation == "reviewer-bot":
                    review_user["type"] = "Bot"
                elif mutation == "reviewer-missing-type":
                    del review_user["type"]
                elif mutation == "configured-id":
                    configured_user["id"] = 77
                elif mutation == "configured-login":
                    configured_user["login"] = "different-owner"
                elif mutation == "configured-team":
                    configured["type"] = "Team"
                elif mutation == "configured-bot":
                    configured_user["type"] = "Bot"
                elif mutation == "configured-missing-type":
                    del configured_user["type"]
                elif mutation == "missing-reviewer":
                    rule["reviewers"] = []
                elif mutation == "additional-reviewer":
                    rule["reviewers"].append({"type": "User", "reviewer": {
                        "id": 77, "login": "foreign-reviewer", "type": "User"}})
                elif mutation == "duplicate-owner-reviewer":
                    rule["reviewers"].append(json.loads(json.dumps(configured)))
                elif mutation == "prevent-self-review":
                    rule["prevent_self_review"] = True
                elif mutation == "missing-self-review-policy":
                    del rule["prevent_self_review"]
                elif mutation == "numeric-self-review-policy":
                    rule["prevent_self_review"] = 0
                elif mutation in ("actor-owner-id-only", "rerun-owner-id-only"):
                    run["actor" if mutation.startswith("actor") else "triggering_actor"]["id"] = owner["id"]
                elif mutation in ("actor-owner-login-only", "rerun-owner-login-only"):
                    run["actor" if mutation.startswith("actor") else "triggering_actor"]["login"] = owner["login"]
                elif mutation == "missing-actor":
                    del run["actor"]
                elif mutation == "missing-rerun-actor":
                    del run["triggering_actor"]
                elif mutation == "invalid-actor-id":
                    run["actor"]["id"] = True
                elif mutation == "invalid-rerun-login":
                    run["triggering_actor"]["login"] = "bad login"
                else:
                    run["triggering_actor"]["id"] = run["actor"]["id"]
                with self.assertRaises(self.physical_receipt.ContractError):
                    self.physical_receipt.validate(**fixture)

    def test_physical_receipt_cli_hashes_exact_written_bytes(self) -> None:
        fixture = self._physical_receipt_fixture()
        envelope = self.physical_receipt.validate(**fixture)
        self.assertEqual(envelope["environmentId"], 41)
        self.assertEqual(envelope["review"]["userId"], 12602174)
        self.assertEqual(envelope["review"]["user"], "TheWhykiki")
        self.assertEqual(envelope["branchProtection"]["candidateAncestor"], TAG_COMMIT)
        self.assertEqual(
            envelope["branchProtection"]["workflowAncestor"], fixture["workflow_sha"]
        )
        expected = json.dumps(
            envelope, ensure_ascii=False, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            paths = {}
            for key in (
                "environment",
                "reviews",
                "workflow_run",
                "repository_metadata",
                "branch",
                "candidate_compare",
                "workflow_compare",
                "release",
            ):
                path = directory / f"{key}.json"
                path.write_text(json.dumps(fixture[key]), encoding="utf-8")
                paths[key] = path
            output = directory / "receipt.json"
            evidence_directory = directory / "evidence"
            evidence_directory.mkdir()
            for name, encoded in fixture["evidence_files"].items():
                (evidence_directory / name).write_bytes(encoded)
            command = [
                "python3",
                str(ROOT / "scripts" / "validate-physical-daw-release-receipt.py"),
                "--environment-json", str(paths["environment"]),
                "--reviews-json", str(paths["reviews"]),
                "--workflow-run-json", str(paths["workflow_run"]),
                "--repository-json", str(paths["repository_metadata"]),
                "--branch-json", str(paths["branch"]),
                "--candidate-compare-json", str(paths["candidate_compare"]),
                "--workflow-compare-json", str(paths["workflow_compare"]),
                "--release-json", str(paths["release"]),
                "--repository", str(fixture["repository"]),
                "--product", str(fixture["product"]),
                "--environment", str(fixture["environment_name"]),
                "--run-id", str(fixture["run_id"]),
                "--run-attempt", str(fixture["run_attempt"]),
                "--workflow-sha", str(fixture["workflow_sha"]),
                "--release-id", str(fixture["release_id"]),
                "--tag", str(fixture["tag"]),
                "--commit", str(fixture["commit"]),
                "--asset-manifest-sha256", str(fixture["asset_manifest_sha256"]),
                "--evidence-directory", str(evidence_directory),
                "--output", str(output),
            ]
            completed = subprocess.run(command, check=False, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(completed.stdout.strip(), hashlib.sha256(expected).hexdigest())

    def test_physical_receipt_rejects_bypass_and_incomplete_matrix(self) -> None:
        for mutation in (
            "foreign-reviewer",
            "actor-id-reuse",
            "unprotected-environment",
            "unprotected-default-branch",
            "candidate-diverged",
            "workflow-not-ancestor",
            "nondefault-branch",
            "extra-review",
            "missing-reaper",
            "digest",
            "missing-cpu",
            "rosetta",
            "old-binary",
            "wrong-loaded-path",
            "old-schema",
            "tampered-evidence",
            "missing-evidence",
            "admin-bypass",
            "missing-admin-policy",
            "mixed-environment-review",
            "physical-id-name-mismatch",
        ):
            with self.subTest(mutation=mutation):
                fixture = self._physical_receipt_fixture()
                if mutation == "foreign-reviewer":
                    fixture["reviews"][0]["user"] = {"id": 77, "login": "admin-bypass"}
                elif mutation == "actor-id-reuse":
                    fixture["workflow_run"]["actor"]["id"] = fixture["reviews"][0]["user"]["id"]
                elif mutation == "unprotected-environment":
                    fixture["environment"]["deployment_branch_policy"] = {
                        "protected_branches": False,
                        "custom_branch_policies": False,
                    }
                elif mutation == "unprotected-default-branch":
                    fixture["branch"]["protected"] = False
                elif mutation == "candidate-diverged":
                    fixture["candidate_compare"]["status"] = "diverged"
                elif mutation == "workflow-not-ancestor":
                    fixture["workflow_compare"]["merge_base_commit"]["sha"] = "3" * 40
                elif mutation == "nondefault-branch":
                    fixture["workflow_run"]["head_branch"] = "release-candidate"
                elif mutation == "extra-review":
                    fixture["reviews"].append(json.loads(json.dumps(fixture["reviews"][0])))
                elif mutation == "admin-bypass":
                    fixture["environment"]["can_admins_bypass"] = True
                elif mutation == "missing-admin-policy":
                    del fixture["environment"]["can_admins_bypass"]
                elif mutation == "mixed-environment-review":
                    fixture["reviews"][0]["environments"].append({"id": 99, "name": "release-signing"})
                elif mutation == "physical-id-name-mismatch":
                    fixture["reviews"][0]["environments"][0]["name"] = "release-signing"
                elif mutation == "tampered-evidence":
                    name = next(iter(fixture["evidence_files"]))
                    fixture["evidence_files"][name] += b" "
                elif mutation == "missing-evidence":
                    fixture["evidence_files"].pop(next(iter(fixture["evidence_files"])))
                else:
                    receipt = json.loads(fixture["reviews"][0]["comment"])
                    if mutation == "missing-reaper":
                        receipt["checks"].pop()
                    elif mutation == "missing-cpu":
                        del receipt["checks"][0]["cpuArchitecture"]
                    elif mutation == "rosetta":
                        check = next(item for item in receipt["checks"] if item["platform"].startswith("macos")
                                     and item["cpuArchitecture"] == "arm64")
                        check["hostProcessArchitecture"] = "x86_64"
                    elif mutation == "old-binary":
                        receipt["checks"][0]["loadedVst3Sha256"] = "f" * 64
                    elif mutation == "wrong-loaded-path":
                        receipt["checks"][0]["loadedVst3Path"] = "relative/other.vst3"
                    elif mutation == "old-schema":
                        receipt["schemaVersion"] = 1
                    else:
                        first_name = next(iter(receipt["artifacts"]))
                        receipt["artifacts"][first_name] = f"sha256:{'f' * 64}"
                    fixture["reviews"][0]["comment"] = json.dumps(receipt)
                with self.assertRaises(self.physical_receipt.ContractError):
                    self.physical_receipt.validate(**fixture)

    def test_physical_receipt_allows_separate_signing_approvals(self) -> None:
        fixture = self._physical_receipt_fixture()
        expected = self.physical_receipt.validate(**fixture)
        signing_review = {
            "state": "approved", "comment": "Sign the reviewed candidate",
            "environments": [{"id": 99, "name": "release-signing"}],
            "user": {"id": 12602174, "login": "TheWhykiki", "type": "User"},
        }
        fixture["reviews"].insert(0, signing_review)
        fixture["reviews"].append(json.loads(json.dumps(signing_review)))
        self.assertEqual(self.physical_receipt.validate(**fixture), expected)

    def test_macos_candidate_is_signed_notarized_and_required_for_publish(self) -> None:
        product_prefix = PRODUCT.upper()
        for token in (
            "build-macos:",
            "runs-on: macos-15",
            "needs: [authorize_windows_release, build-windows, build-macos, test-macos-intel-candidate, source-release]",
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
            "scripts/verify-macos-signers.py",
            '--next-installer-cert-sha256 "$NEXT_INSTALLER_SIGNER_SHA256"',
            "codesign --verify --deep --strict",
            '--next-application-cert-sha256 "$NEXT_APPLICATION_SIGNER_SHA256"',
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
        self.assertEqual(self.release.count("\n      APPLICATION_SIGNER_SHA256:"), 3)
        self.assertEqual(self.release.count("\n      INSTALLER_SIGNER_SHA256:"), 3)
        self.assertEqual(self.release.count("\n      NEXT_PROFILE_EKU:"), 3)
        self.assertGreaterEqual(self.release.count("timeout-minutes: 120"), 2)
        self.assertNotIn(f"{PRODUCT}-$version-Windows-SHA256SUMS.txt", self.release)
        self.assertLess(
            self.release.index("unset APPLICATION_P12_BASE64"),
            self.release.index('./scripts/package-release.sh Release "$WK_RELEASE_VERSION"'),
        )
        stage = self.release[self.release.index("stage-release:") :]
        self.assertIn("macOS-universal", stage)
        self.assertIn("Windows-arm64ec", stage)

    def test_exact_macos_candidate_is_host_loaded_on_native_intel_before_publish(self) -> None:
        start = self.release.index("  test-macos-intel-candidate:")
        end = self.release.index("\n  stage-release:", start)
        intel_gate = self.release[start:end]
        for token in (
            "needs: build-macos",
            "needs.build-macos.result == 'success'",
            "runs-on: macos-15-intel",
            "timeout-minutes: 45",
            "permissions:\n      contents: read",
            "test \"$RUNNER_ARCH\" = 'X64'",
            "test \"$(uname -m)\" = 'x86_64'",
            "ref: ${{ inputs.tag }}",
            "persist-credentials: false",
            "git ls-remote --exit-code origin",
            "CMAKE_PROJECT_VERSION:STATIC",
            f"--target {PRODUCT}HostTests",
            f"name: {PRODUCT}-macOS-universal-SIGNED-NOTARIZED-RELEASE-CANDIDATE-${{{{ github.run_id }}}}-${{{{ github.run_attempt }}}}",
            "scripts/macos-release-assets.py validate",
            "pkgutil --expand-full",
            f'zip_bundle="$roundtrip/zip/{PRODUCT}.vst3"',
            f'package_bundle="$roundtrip/package/Payload/Library/Audio/Plug-Ins/VST3/{PRODUCT}.vst3"',
            "for source in zip package; do",
            'package) signed_bundle="$package_bundle" ;;',
            'scripts/verify-macos-signers.py --package "$package" --bundle "$signed_bundle"',
            f'host="build-macos-intel-host/{PRODUCT}HostTests_artefacts/Release/{PRODUCT}HostTests"',
            '/usr/bin/arch -x86_64 "$host" "$zip_bundle"',
            '/usr/bin/arch -x86_64 "$host" "$package_bundle"',
            "candidate_sha256: ${{ steps.native_intel_gate.outputs.candidate_sha256 }}",
            "printf 'candidate_sha256=%s\\n' \"$candidate_digest\" >> \"$GITHUB_OUTPUT\"",
        ):
            self.assertIn(token, intel_gate)
        self.assertNotIn("${{ secrets.", intel_gate)
        self.assertNotIn("WK_RUNNER_ARCH: ${{ runner.arch }}", intel_gate)

        stage = self.release[end:]
        for token in (
            "needs: [authorize_windows_release, build-windows, build-macos, test-macos-intel-candidate, source-release]",
            "needs.test-macos-intel-candidate.result == 'success'",
            "WK_INTEL_TESTED_MACOS_CANDIDATE_SHA256: ${{ needs.test-macos-intel-candidate.outputs.candidate_sha256 }}",
            'tested_candidate_digest="$WK_INTEL_TESTED_MACOS_CANDIDATE_SHA256"',
            'test "$actual_candidate_digest" = "$tested_candidate_digest"',
        ):
            self.assertIn(token, stage)
        self.assertIn("runs-on: macos-15\n    timeout-minutes: 30", stage)

    @unittest.skipIf(importlib.util.find_spec("yaml") is None, "PyYAML is unavailable")
    def test_release_workflow_is_valid_yaml(self) -> None:
        import yaml

        parsed = yaml.safe_load(self.release)
        self.assertIsInstance(parsed, dict)
        self.assertIn("jobs", parsed)

    @unittest.skipIf(shutil.which("pwsh") is None, "PowerShell is unavailable")
    def test_transition_gate_has_no_powershell_parser_errors(self) -> None:
        script = ROOT / "scripts" / "test-windows-updater-transition.ps1"
        command = (
            "$path=$env:WK_TRANSITION_SCRIPT_PATH;"
            "if([string]::IsNullOrWhiteSpace($path)){"
            "[Console]::Error.WriteLine('transition script path is missing');exit 2};"
            "$tokens=$null;$errors=$null;"
            "[System.Management.Automation.Language.Parser]::ParseFile("
            "$path,[ref]$tokens,[ref]$errors)>$null;"
            "if($errors.Count){$errors|ForEach-Object{[Console]::Error.WriteLine($_)};exit 1}"
        )
        completed = subprocess.run(
            ["pwsh", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command],
            check=False,
            capture_output=True,
            env={**os.environ, "WK_TRANSITION_SCRIPT_PATH": str(script)},
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_all_external_actions_are_full_commit_pinned(self) -> None:
        references = re.findall(r"(?m)^\s*-?\s*uses:\s*[^@\s]+@([^\s#]+)", self.release)
        self.assertTrue(references)
        self.assertTrue(all(re.fullmatch(r"[0-9a-f]{40}", reference) for reference in references))

    def test_unsigned_ci_vst3_file_and_container_names_are_exact(self) -> None:
        expected = f"{PRODUCT}-Windows-${{{{ matrix.artifact_arch }}}}-VST3-UNSIGNED-NOT-FOR-DISTRIBUTION"
        self.assertGreaterEqual(self.ci.count(expected), 2)
        self.assertNotIn(f"{PRODUCT}-VST3-Windows-", self.ci)

    def _write_candidate(
        self,
        directory: pathlib.Path,
        architecture: str,
        next_signer: str = NEXT_SIGNER,
        next_release_gate_public_key: str = RELEASE_GATE_NEXT_PUBLIC_KEY,
    ) -> pathlib.Path:
        version = "1.2.3"
        base = f"{PRODUCT}-{version}-Windows-{architecture}"
        msi = directory / f"{base}.msi"
        msi.write_bytes(f"signed-msi-{architecture}".encode("ascii"))
        package_config = json.loads(
            (ROOT / "Installer/Windows/package-config.json").read_text(encoding="utf-8")
        )
        other_architecture = "arm64ec" if architecture == "x64" else "x64"
        evidence = {
            "schemaVersion": 5,
            "releaseContractVersion": 2,
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
            "signerCertificateSha256": "A1" * 32,
            "signingProfileEku": SIGNER,
            "timestampCertificateSha256": "C4" * 32,
            "payloadSigningEvidence": [
                {"path": path, "profileEku": SIGNER, "signerCertificateSha256": "A1" * 32,
                 "timestampCertificateSha256": "C4" * 32}
                for path in [
                    f"Contents\\Helpers\\{PRODUCT}Updater.exe",
                    f"Contents\\{'x86_64-win' if architecture == 'x64' else 'arm64ec-win'}\\{PRODUCT}.vst3",
                ]
            ],
            "updaterCurrentProfileEku": SIGNER,
            "updaterNextProfileEku": next_signer or None,
            "payloadProfileEkuAllowlist": [SIGNER] + ([next_signer] if next_signer else []),
            "releaseGatePublicKeyXY": RELEASE_GATE_PUBLIC_KEY,
            "releaseGateNextPublicKeyXY": next_release_gate_public_key or None,
            "releaseGatePublicKeyAllowlistXY": [RELEASE_GATE_PUBLIC_KEY]
            + ([next_release_gate_public_key] if next_release_gate_public_key else []),
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
                "sequenceMutationTests": 14,
                "nativeUpdaterSequenceMutationTests": 14,
                "architectureMutationTests": 1,
                "nativeUpdaterArchitectureMutationTests": 1,
                "installExecuteSequence": [
                    {"action": action, "condition": "", "sequence": str(number)}
                    for action, number in (
                        ("FindRelatedProducts", 50), ("LaunchConditions", 100),
                        ("MigrateFeatureStates", 1200), ("InstallInitialize", 1500),
                        ("RemoveExistingProducts", 1501), ("InstallFiles", 4000))
                ],
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
                    "NOTWIX_DOWNGRADE_DETECTED",
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
                directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",)
            )
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["msiSha256"] = "00" * 32
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                    RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",)
                )

    def test_asset_validator_rejects_unsafe_recorded_msi_sequences(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            evidence_path = self._write_candidate(directory, "x64")
            baseline = json.loads(evidence_path.read_text(encoding="utf-8"))
            sequence = baseline["validation"]["installExecuteSequence"]
            mutants = [sequence[1:], sequence + [sequence[0]]]
            for field, value in (("condition", "0"), ("sequence", ""), ("sequence", "0"),
                                 ("sequence", "-1"), ("sequence", None), ("sequence", "4001")):
                mutant = json.loads(json.dumps(sequence))
                mutant[0][field] = value
                mutants.append(mutant)
            migration = json.loads(json.dumps(sequence))
            migration[2]["sequence"] = "1"
            mutants.append(migration)
            for index, mutant in enumerate(mutants):
                with self.subTest(mutant=index):
                    baseline["validation"]["installExecuteSequence"] = mutant
                    evidence_path.write_text(json.dumps(baseline), encoding="utf-8")
                    with self.assertRaises(self.validator.ContractError):
                        self.validator.validate_assets(
                            directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                            RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",))

    def test_asset_validator_rejects_missing_other_architecture(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self._write_candidate(directory, "x64")
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                    RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY,
                    ("x64", "arm64ec")
                )

    def test_asset_validator_rejects_a_different_source_commit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self._write_candidate(directory, "x64")
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    directory, PRODUCT, "1.2.3", "0" * 40, SIGNER, NEXT_SIGNER,
                    RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",)
                )

    def test_asset_validator_rejects_out_of_range_msi_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    pathlib.Path(temporary), PRODUCT, "256.0.0", TAG_COMMIT, SIGNER,
                    NEXT_SIGNER, RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY,
                    ("x64",)
                )

    def test_asset_validator_rejects_incomplete_updater_and_msi_policy_evidence(self) -> None:
        mutations = (
            ("updaterVersionResource", "originalFilename", "WrongUpdater.exe"),
            ("validation", "nativeUpdaterSequenceMutationTests", 0),
            ("validation", "nativeUpdaterArchitectureMutationTests", 0),
            ("validation", "installExecuteSequence", []),
            ("validation", "msiDeploymentCompliant", "0"),
            ("validation", "secureCustomProperties", ["WIX_UPGRADE_DETECTED"]),
            (
                "validation",
                "launchConditions",
                [
                    "INSTALLEDORNOTOTHERARCHITECTUREDETECTED",
                    "INSTALLEDORNOTWIX_DOWNGRADE_DETECTED",
                ],
            ),
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
                        directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                        RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",)
                    )

    def test_asset_validator_binds_one_or_two_unique_ordered_signer_pins(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self._write_candidate(directory, "x64", "")
            self.validator.validate_assets(
                directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, "",
                RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",)
            )
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            evidence_path = self._write_candidate(directory, "x64")
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            evidence["payloadProfileEkuAllowlist"] = [NEXT_SIGNER, SIGNER]
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                    RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",)
                )
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    pathlib.Path(temporary), PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, SIGNER,
                    RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",)
                )

    def test_asset_validator_binds_exact_ordered_release_gate_keys_for_both_architectures(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            self._write_candidate(directory, "x64")
            self._write_candidate(directory, "arm64ec")
            self.validator.validate_assets(
                directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY,
                ("x64", "arm64ec"),
            )

        mutations = (
            ("releaseGatePublicKeyXY", RELEASE_GATE_NEXT_PUBLIC_KEY),
            ("releaseGateNextPublicKeyXY", None),
            (
                "releaseGatePublicKeyAllowlistXY",
                [RELEASE_GATE_NEXT_PUBLIC_KEY, RELEASE_GATE_PUBLIC_KEY],
            ),
        )
        for field, value in mutations:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                directory = pathlib.Path(temporary)
                evidence_path = self._write_candidate(directory, "x64")
                evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
                evidence[field] = value
                evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
                with self.assertRaises(self.validator.ContractError):
                    self.validator.validate_assets(
                        directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                        RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_NEXT_PUBLIC_KEY, ("x64",),
                    )

        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            evidence_path = self._write_candidate(
                directory, "x64", next_release_gate_public_key=""
            )
            self.validator.validate_assets(
                directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                RELEASE_GATE_PUBLIC_KEY, "", ("x64",),
            )
            evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
            del evidence["releaseGateNextPublicKeyXY"]
            evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
            with self.assertRaises(self.validator.ContractError):
                self.validator.validate_assets(
                    directory, PRODUCT, "1.2.3", TAG_COMMIT, SIGNER, NEXT_SIGNER,
                    RELEASE_GATE_PUBLIC_KEY, "", ("x64",),
                )

        for current, next_key in (
            (RELEASE_GATE_PUBLIC_KEY.lower(), RELEASE_GATE_NEXT_PUBLIC_KEY),
            (RELEASE_GATE_PUBLIC_KEY, RELEASE_GATE_PUBLIC_KEY),
        ):
            with self.subTest(current=current, next_key=next_key), tempfile.TemporaryDirectory() as temporary:
                with self.assertRaises(self.validator.ContractError):
                    self.validator.validate_assets(
                        pathlib.Path(temporary), PRODUCT, "1.2.3", TAG_COMMIT,
                        SIGNER, NEXT_SIGNER, current, next_key, ("x64",),
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
            "WINDOWS_ARTIFACT_SIGNING_PROFILE_EKU",
            "AZURE_CLIENT_ID",
            "WINDOWS_RELEASE_GATE_KEY_VAULT_KEY_ID",
            "http://timestamp.acs.microsoft.com/",
            "MACOS_DEVELOPER_ID_APPLICATION_P12_BASE64",
            "MACOS_DEVELOPER_ID_INSTALLER_P12_BASE64",
            "MACOS_NOTARY_PRIVATE_KEY_P8_BASE64",
            "MACOS_DEVELOPER_ID_APPLICATION_CERT_SHA256",
            "MACOS_DEVELOPER_ID_INSTALLER_CERT_SHA256",
            "confirm_release",
            "ARM64EC",
            "Draft",
            "physical-daw-release",
            "prevent_self_review=false",
            "Allow administrators to bypass configured protection rules",
            "GET /repos/{owner}/{repo}/actions/runs/{run_id}/approvals",
            "Cubase",
            "Reaper",
            "macos-universal-pkg",
            "macos-universal-zip",
        ):
            self.assertIn(token, self.docs)
        self.assertIn("veröffentlicht jetzt nichts", self.docs)


if __name__ == "__main__":
    unittest.main()
