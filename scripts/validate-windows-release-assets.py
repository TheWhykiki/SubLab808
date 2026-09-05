#!/usr/bin/env python3
"""Fail-closed validation for signed Windows release candidates."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


ARCHITECTURES = {
    "x64": "x64",
    "arm64ec": "arm64",
}
SHA256_RE = re.compile(r"^[0-9A-F]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
GUID_RE = re.compile(r"^[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}$")
VERSION_RE = re.compile(r"^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$")
REQUIRED_VALIDATIONS = (
    "moduleInfoIdentityValidated",
    "pluginVersionResourceValidated",
    "updaterVersionResourceValidated",
    "payloadClassificationValidated",
    "wixIce",
    "directoryGraphValidated",
    "componentContainmentValidated",
    "fileComponentReferencesValidated",
    "administrativeExtractionHashMatch",
    "administrativeImageLayoutValidated",
    "hostTestRan",
)


class ContractError(RuntimeError):
    """Raised when a release candidate violates the public asset contract."""


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def _validate_version(version: str) -> None:
    _require(VERSION_RE.fullmatch(version) is not None, "Version is not a canonical MSI version")
    major, minor, patch = (int(part) for part in version.split("."))
    _require(major <= 255 and minor <= 255 and patch <= 65535, "Version exceeds Windows Installer bounds")


def _load_evidence(path: pathlib.Path) -> dict[str, Any]:
    _require(path.stat().st_size <= 4 * 1024 * 1024, f"Evidence is unexpectedly large: {path.name}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ContractError(f"Evidence is not canonical UTF-8 JSON: {path.name}: {error}") from error
    _require(isinstance(value, dict), f"Evidence root must be an object: {path.name}")
    return value


def _validate_payload_files(evidence: dict[str, Any], product: str) -> None:
    payload = evidence.get("payloadFiles")
    _require(isinstance(payload, list) and payload, "Evidence must contain a non-empty payloadFiles array")
    seen: set[str] = set()
    helper = f"contents\\helpers\\{product.lower()}updater.exe"
    helper_seen = False
    for entry in payload:
        _require(isinstance(entry, dict), "Every payloadFiles entry must be an object")
        path = entry.get("path")
        size = entry.get("size")
        digest = entry.get("sha256")
        _require(isinstance(path, str) and path, "Every payload file needs a path")
        _require("/" not in path and "\x00" not in path, f"Unsafe payload path: {path!r}")
        parts = path.split("\\")
        _require(
            all(part not in ("", ".", "..") for part in parts) and ":" not in parts[0],
            f"Unsafe payload path: {path!r}",
        )
        folded = path.casefold()
        _require(folded not in seen, f"Duplicate/case-colliding payload path: {path}")
        seen.add(folded)
        _require(isinstance(size, int) and not isinstance(size, bool) and size > 0, f"Invalid payload size: {path}")
        _require(isinstance(digest, str) and SHA256_RE.fullmatch(digest) is not None, f"Invalid payload SHA-256: {path}")
        helper_seen |= folded == helper
    _require(helper_seen, "Evidence does not include the exact production updater helper")


def _validate_identity_evidence(
    evidence: dict[str, Any], product: str, version: str, architecture: str
) -> None:
    config_path = pathlib.Path(__file__).resolve().parents[1] / "Installer" / "Windows" / "package-config.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    _require(config.get("productName") == product, "Repository installer identity does not match the product")
    manufacturer = config.get("manufacturer")
    configured_entries = config.get("vst3Classes")
    _require(isinstance(configured_entries, list), "Installer VST3 class configuration is invalid")
    configured_classes = {f"{entry['cid']}|{entry['category']}" for entry in configured_entries}
    module = evidence.get("moduleInfo")
    _require(isinstance(module, dict), "moduleInfo evidence is missing")
    _require(module.get("path") == "Contents\\Resources\\moduleinfo.json", "moduleInfo path is invalid")
    _require(module.get("name") == product, "moduleInfo product is invalid")
    _require(module.get("manufacturer") == manufacturer, "moduleInfo manufacturer is invalid")
    _require(module.get("version") == version, "moduleInfo version is invalid")
    _require(SHA256_RE.fullmatch(str(module.get("sha256", ""))) is not None, "moduleInfo hash is invalid")
    class_identities = module.get("classIdentities")
    _require(isinstance(class_identities, list) and all(isinstance(value, str) for value in class_identities),
             "moduleInfo class identities are invalid")
    _require(len(class_identities) == 2 and set(class_identities) == configured_classes and len(configured_classes) == 2,
             "moduleInfo class identities do not match package-config.json")

    resource = evidence.get("pluginVersionResource")
    _require(isinstance(resource, dict), "pluginVersionResource evidence is missing")
    expected_resource = {
        "fileVersion": version,
        "productVersion": version,
        "companyName": manufacturer,
        "productName": product,
        "fileDescription": product,
    }
    for key, expected in expected_resource.items():
        _require(resource.get(key) == expected, f"Plugin version-resource field {key!r} is invalid")
    _require(resource.get("mutationTests") == 3, "Plugin version-resource mutation tests did not pass")

    updater_resource = evidence.get("updaterVersionResource")
    _require(isinstance(updater_resource, dict), "updaterVersionResource evidence is missing")
    expected_updater_resource = {
        "fileVersion": version,
        "productVersion": version,
        "companyName": manufacturer,
        "productName": product,
        "fileDescription": f"{product} Updater",
        "internalName": f"{product}Updater",
        "originalFilename": f"{product}Updater.exe",
    }
    for key, expected in expected_updater_resource.items():
        _require(updater_resource.get(key) == expected,
                 f"Updater version-resource field {key!r} is invalid")
    _require(updater_resource.get("mutationTests") == 3,
             "Updater version-resource mutation tests did not pass")

    classification = evidence.get("payloadClassification")
    _require(isinstance(classification, dict), "payloadClassification evidence is missing")
    portable = classification.get("portableExecutablePaths")
    signed = classification.get("signedPortableExecutablePaths")
    helper = f"Contents\\Helpers\\{product}Updater.exe"
    plugin = (
        f"Contents\\{'x86_64-win' if architecture == 'x64' else 'arm64ec-win'}\\{product}.vst3"
    )
    _require(isinstance(portable, list) and portable and all(isinstance(value, str) for value in portable)
             and len(portable) == len(set(portable)),
             "Portable-executable classification is empty or contains duplicates")
    _require(isinstance(signed, list) and all(isinstance(value, str) for value in signed)
             and len(signed) == len(set(signed)) and set(signed) == set(portable),
             "Not every classified PE was signed")
    _require(plugin in portable and helper in portable, "Primary VST3 or production updater is not classified as PE")
    updater_like = classification.get("updaterLikePortableExecutablePaths")
    helper_paths = classification.get("helperPortableExecutablePaths")
    _require(updater_like == [helper],
             "Updater-like PE classification is not exact")
    _require(isinstance(helper_paths, list) and helper in helper_paths,
             "Production updater is not classified as a helper PE")
    for key in ("forbiddenExecutableExtensions", "malformedExecutableExtensions"):
        _require(classification.get(key) == 0, f"Payload classification field {key!r} is nonzero")
    _require(classification.get("everyPortableExecutableArchitectureValidated") is True,
             "Not every PE architecture was validated")

    payload_by_path = {
        entry.get("path"): entry for entry in evidence.get("payloadFiles", []) if isinstance(entry, dict)
    }
    _require(set(portable).issubset(payload_by_path), "Classified PE inventory is absent from payloadFiles")
    _require(payload_by_path.get(module["path"], {}).get("sha256") == module.get("sha256"),
             "moduleInfo hash is not bound to payloadFiles")


def validate_assets(
    directory: pathlib.Path,
    product: str,
    version: str,
    source_commit: str,
    expected_signer_sha256: str,
    architectures: tuple[str, ...],
) -> None:
    _require(directory.is_dir() and not directory.is_symlink(), f"Asset directory is invalid: {directory}")
    _require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", product) is not None, "Unsafe product name")
    _validate_version(version)
    _require(COMMIT_RE.fullmatch(source_commit) is not None,
             "Source commit must be exactly 40 lowercase hexadecimal characters")
    signer = expected_signer_sha256.replace(" ", "").upper()
    _require(SHA256_RE.fullmatch(signer) is not None, "Expected signer must be exactly 64 hexadecimal characters")
    _require(architectures and len(set(architectures)) == len(architectures), "Architectures must be unique")

    expected_names: set[str] = set()
    for architecture in architectures:
        _require(architecture in ARCHITECTURES, f"Unsupported architecture: {architecture}")
        base = f"{product}-{version}-Windows-{architecture}"
        expected_names.update((f"{base}.msi", f"{base}.evidence.json"))

    entries = list(directory.iterdir())
    for entry in entries:
        _require(entry.is_file() and not entry.is_symlink(), f"Nested, special or linked asset is forbidden: {entry.name}")
    actual_names = {entry.name for entry in entries}
    _require(actual_names == expected_names, f"Unexpected release asset set: {sorted(actual_names)}")

    for architecture in architectures:
        base = f"{product}-{version}-Windows-{architecture}"
        msi = directory / f"{base}.msi"
        evidence_path = directory / f"{base}.evidence.json"
        _require(msi.stat().st_size > 0, f"MSI is empty: {msi.name}")
        _require(msi.stat().st_size <= 256 * 1024 * 1024, f"MSI exceeds updater size policy: {msi.name}")
        evidence = _load_evidence(evidence_path)
        expected_values = {
            "schemaVersion": 2,
            "artifactStatus": "SIGNED",
            "product": product,
            "version": version,
            "sourceCommit": source_commit,
            "payloadArchitecture": architecture,
            "msiArchitecture": ARCHITECTURES[architecture],
            "msiFile": msi.name,
            "signed": True,
            "signerCertificateSha256": signer,
            "updaterSignerPinSha256": signer,
        }
        for key, expected in expected_values.items():
            _require(evidence.get(key) == expected, f"Evidence field {key!r} is invalid for {architecture}")
        config = json.loads(
            (pathlib.Path(__file__).resolve().parents[1] / "Installer/Windows/package-config.json").read_text(
                encoding="utf-8"
            )
        )
        other_architecture = "arm64ec" if architecture == "x64" else "x64"
        _require(evidence.get("upgradeCode") == config["upgradeCodes"][architecture],
                 f"UpgradeCode is invalid for {architecture}")
        _require(evidence.get("otherArchitectureUpgradeCode") == config["upgradeCodes"][other_architecture],
                 f"Other-architecture UpgradeCode is invalid for {architecture}")
        _require(GUID_RE.fullmatch(str(evidence.get("productCode", ""))) is not None,
                 f"ProductCode is invalid for {architecture}")
        _require(evidence.get("wixVersion") == "6.0.2", f"Unexpected WiX version for {architecture}")
        for key in ("wixToolManifestSha256", "nuGetConfigSha256", "timestampUrlSha256"):
            _require(SHA256_RE.fullmatch(str(evidence.get(key, ""))) is not None,
                     f"Evidence field {key!r} is invalid for {architecture}")
        _require(evidence.get("signingDigest") == "SHA256", f"Signing digest is invalid for {architecture}")
        _require(evidence.get("timestampProtocol") == "RFC3161-SHA256",
                 f"Timestamp protocol is invalid for {architecture}")
        recorded_hash = evidence.get("msiSha256")
        _require(isinstance(recorded_hash, str) and SHA256_RE.fullmatch(recorded_hash) is not None,
                 f"Evidence MSI hash is invalid for {architecture}")
        _require(_sha256(msi) == recorded_hash, f"MSI hash mismatch for {architecture}")
        _require(
            evidence.get("updaterPaths") == [f"Contents\\Helpers\\{product}Updater.exe"],
            f"Updater path contract is invalid for {architecture}",
        )
        validation = evidence.get("validation")
        _require(isinstance(validation, dict), f"Validation evidence is missing for {architecture}")
        for key in REQUIRED_VALIDATIONS:
            _require(validation.get(key) is True, f"Required validation {key!r} did not pass for {architecture}")
        _require(validation.get("customActions") == 0, f"Custom actions are present for {architecture}")
        _require(validation.get("forbiddenSideEffectTables") == 0,
                 f"Forbidden side-effect tables are present for {architecture}")
        _require(validation.get("forbiddenSequenceActions") == 0,
                 f"Forbidden sequence actions are present for {architecture}")
        _require(isinstance(validation.get("policyMutationTests"), int) and
                 validation["policyMutationTests"] >= 12,
                 f"Installer policy mutation tests did not pass for {architecture}")
        display_name = f"{product} VST3 - " + (
            "Windows x64" if architecture == "x64" else "Windows on Arm (ARM64EC)"
        )
        _require(validation.get("displayName") == display_name,
                 f"MSI display-name evidence is invalid for {architecture}")
        _require("productName" not in validation,
                 f"Ambiguous legacy MSI product-name evidence is forbidden for {architecture}")
        _require(validation.get("manufacturer") == "Whykiki Audio",
                 f"MSI manufacturer evidence is invalid for {architecture}")
        _require(validation.get("productLanguage") == "1033",
                 f"MSI language evidence is invalid for {architecture}")
        _require(validation.get("msiDeploymentCompliant") == "1",
                 f"MSIDEPLOYMENTCOMPLIANT evidence is invalid for {architecture}")
        secure_properties = validation.get("secureCustomProperties")
        _require(isinstance(secure_properties, list) and
                 len(secure_properties) == 3 and
                 set(secure_properties) == {
            "WIX_UPGRADE_DETECTED",
            "WIX_DOWNGRADE_DETECTED",
            "OTHERARCHITECTUREDETECTED",
        }, f"SecureCustomProperties evidence is invalid for {architecture}")
        launch_conditions = validation.get("launchConditions")
        _require(isinstance(launch_conditions, list) and len(launch_conditions) == 2 and
                 set(launch_conditions) == {
            "INSTALLEDORNOTWIX_DOWNGRADE_DETECTED",
            "INSTALLEDORNOTOTHERARCHITECTUREDETECTED",
        }, f"MSI launch-condition evidence is invalid for {architecture}")
        _validate_payload_files(evidence, product)
        _validate_identity_evidence(evidence, product, version, architecture)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", required=True, type=pathlib.Path)
    parser.add_argument("--product", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--expected-signer-sha256", required=True)
    parser.add_argument("--architecture", choices=tuple(ARCHITECTURES), action="append")
    args = parser.parse_args()
    architectures = tuple(args.architecture or ARCHITECTURES)
    try:
        validate_assets(
            args.directory,
            args.product,
            args.version,
            args.source_commit,
            args.expected_signer_sha256,
            architectures,
        )
    except (ContractError, OSError) as error:
        print(f"Windows release asset validation failed: {error}", file=sys.stderr)
        return 1
    print("Validated signed Windows release assets: " + ", ".join(architectures))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
