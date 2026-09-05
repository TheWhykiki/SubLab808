#!/usr/bin/env python3
"""Prepare and validate the signed/notarized macOS release asset contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import sys
import uuid
from typing import Any


SHA256_RE = re.compile(r"^[0-9A-F]{64}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
VERSION_RE = re.compile(r"^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$")
PRODUCT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
REQUIRED_VERIFICATION = {
    "generated_factory_bank_matches_snapshot",
    "fresh_snapshot_build",
    "ctest",
    "zip_host_state_audio_roundtrip",
    "installer_payload_host_state_audio_roundtrip",
    "both_macos11_slices",
    "notary_log_accepted",
    "gatekeeper_package",
    "gatekeeper_zip_vst3",
}


class ContractError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def load_json(path: pathlib.Path, maximum: int = 4 * 1024 * 1024) -> dict[str, Any]:
    require(path.is_file() and not path.is_symlink(), f"Missing or linked JSON evidence: {path}")
    require(path.stat().st_size <= maximum, f"JSON evidence is unexpectedly large: {path.name}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ContractError(f"Invalid UTF-8 JSON evidence: {path.name}: {error}") from error
    require(isinstance(value, dict), f"JSON evidence root is not an object: {path.name}")
    return value


def normalize_pin(value: str, description: str) -> str:
    pin = value.replace(" ", "").upper()
    require(SHA256_RE.fullmatch(pin) is not None, f"{description} must be a 64-hex SHA-256 fingerprint")
    return pin


def validate_version(version: str) -> None:
    require(VERSION_RE.fullmatch(version) is not None, "Version is not canonical")
    major, minor, patch = (int(part) for part in version.split("."))
    require(major <= 255 and minor <= 255 and patch <= 65535, "Version exceeds installer bounds")


def validate_product(product: str) -> None:
    require(PRODUCT_RE.fullmatch(product) is not None, "Product name is unsafe")


def validate_pipeline_candidate(candidate: pathlib.Path, product: str, version: str, commit: str) -> dict[str, Any]:
    require(candidate.is_dir() and not candidate.is_symlink(), "Pipeline candidate directory is invalid")
    manifest_path = candidate / "release-manifest.json"
    source_path = candidate / "source-manifest.json"
    manifest = load_json(manifest_path)
    source = load_json(source_path)
    require(manifest.get("schema") == 1, "Unsupported pipeline release-manifest schema")
    require(manifest.get("version") == version and manifest.get("configuration") == "Release",
            "Pipeline version/configuration mismatch")
    require(manifest.get("commit") == commit and COMMIT_RE.fullmatch(commit) is not None,
            "Pipeline candidate is not bound to the requested tag commit")
    require(manifest.get("dirty") is False, "Pipeline candidate was built from a dirty source snapshot")
    for key in ("application_signed", "installer_signed", "notarized"):
        require(manifest.get(key) is True, f"Pipeline evidence does not prove {key}")
    verification = manifest.get("verification")
    require(isinstance(verification, list) and REQUIRED_VERIFICATION.issubset(set(verification)),
            "Pipeline verification evidence is incomplete")
    try:
        uuid.UUID(str(manifest.get("notary_submission_id")))
    except ValueError as error:
        raise ContractError("Pipeline evidence lacks a valid notarization submission ID") from error
    source_hash = manifest.get("source_sha256")
    require(isinstance(source_hash, str) and re.fullmatch(r"[0-9a-f]{64}", source_hash) is not None,
            "Pipeline source hash is invalid")
    require(source.get("schema") == 1 and source.get("source_sha256") == source_hash,
            "Source manifest is not bound to the release manifest")
    repositories = source.get("repositories")
    require(isinstance(repositories, list) and repositories, "Source repository evidence is missing")
    repository_paths: set[str] = set()
    for entry in repositories:
        require(isinstance(entry, dict), "Source repository evidence contains a non-object")
        path = entry.get("path")
        require(isinstance(path, str) and path and not path.startswith("/") and
                (path == "." or all(part not in ("", ".", "..") for part in path.split("/"))),
                "Source repository evidence contains an unsafe path")
        require(path not in repository_paths, "Source repository evidence contains duplicate paths")
        repository_paths.add(path)
        require(COMMIT_RE.fullmatch(str(entry.get("commit", ""))) is not None,
                "Source repository evidence contains an invalid commit")
        require(entry.get("dirty") is False, "A source repository/submodule was dirty")
    roots = [entry for entry in repositories if entry.get("path") == "."]
    require(len(roots) == 1 and roots[0].get("commit") == commit and roots[0].get("dirty") is False,
            "Root source repository evidence is invalid")
    for key in ("built_binary_sha256", "packaged_binary_sha256"):
        require(re.fullmatch(r"[0-9a-f]{64}", str(manifest.get(key, ""))) is not None,
                f"Pipeline binary hash is invalid: {key}")

    package_name = f"{product}-{version}-macOS-universal.pkg"
    archive_name = f"{product}-{version}-macOS-universal-VST3.zip"
    artifacts = manifest.get("artifacts")
    require(isinstance(artifacts, dict), "Pipeline artifact hash evidence is missing")
    for name in (package_name, archive_name, "source-manifest.json"):
        path = candidate / name
        require(path.is_file() and not path.is_symlink(), f"Pipeline artifact is missing: {name}")
        recorded = artifacts.get(name)
        require(isinstance(recorded, str) and recorded.upper() == sha256(path),
                f"Pipeline artifact hash mismatch: {name}")
    return manifest


def prepare(
    candidate: pathlib.Path,
    output: pathlib.Path,
    product: str,
    version: str,
    commit: str,
    application_pin: str,
    installer_pin: str,
) -> None:
    validate_product(product)
    validate_version(version)
    app_pin = normalize_pin(application_pin, "Application signer")
    pkg_pin = normalize_pin(installer_pin, "Installer signer")
    manifest = validate_pipeline_candidate(candidate, product, version, commit)
    require(not output.exists() and not output.is_symlink(), "Refusing to overwrite macOS release assets")
    output.mkdir(parents=False)
    try:
        package_name = f"{product}-{version}-macOS-universal.pkg"
        archive_name = f"{product}-{version}-macOS-universal-VST3.zip"
        evidence_name = f"{product}-{version}-macOS-universal.evidence.json"
        shutil.copy2(candidate / package_name, output / package_name)
        shutil.copy2(candidate / archive_name, output / archive_name)
        evidence = {
            "schemaVersion": 1,
            "artifactStatus": "SIGNED-NOTARIZED",
            "product": product,
            "version": version,
            "tag": f"v{version}",
            "commit": commit,
            "dirty": False,
            "sourceSha256": manifest["source_sha256"].upper(),
            "applicationSigned": True,
            "installerSigned": True,
            "notarized": True,
            "applicationSignerSha256": app_pin,
            "installerSignerSha256": pkg_pin,
            "notarySubmissionId": manifest["notary_submission_id"],
            "verification": manifest["verification"],
            "packageFile": package_name,
            "packageSha256": sha256(output / package_name),
            "vst3ZipFile": archive_name,
            "vst3ZipSha256": sha256(output / archive_name),
            "pipelineManifestSha256": sha256(candidate / "release-manifest.json"),
            "sourceManifestSha256": sha256(candidate / "source-manifest.json"),
        }
        (output / evidence_name).write_text(
            json.dumps(evidence, sort_keys=True, indent=2) + "\n", encoding="utf-8"
        )
        validate(output, product, version, commit, app_pin, pkg_pin)
    except Exception:
        shutil.rmtree(output, ignore_errors=True)
        raise


def validate(
    directory: pathlib.Path,
    product: str,
    version: str,
    commit: str,
    application_pin: str,
    installer_pin: str,
) -> None:
    validate_product(product)
    validate_version(version)
    app_pin = normalize_pin(application_pin, "Application signer")
    pkg_pin = normalize_pin(installer_pin, "Installer signer")
    require(COMMIT_RE.fullmatch(commit) is not None, "Expected commit must be a full lowercase Git SHA")
    require(directory.is_dir() and not directory.is_symlink(), "macOS release asset directory is invalid")
    package_name = f"{product}-{version}-macOS-universal.pkg"
    archive_name = f"{product}-{version}-macOS-universal-VST3.zip"
    evidence_name = f"{product}-{version}-macOS-universal.evidence.json"
    expected = {package_name, archive_name, evidence_name}
    entries = list(directory.iterdir())
    require(all(path.is_file() and not path.is_symlink() for path in entries),
            "Nested, special or linked macOS release assets are forbidden")
    require({path.name for path in entries} == expected, "macOS release asset set is not exact")
    package = directory / package_name
    archive = directory / archive_name
    require(0 < package.stat().st_size <= 128 * 1024 * 1024, "PKG violates updater size policy")
    require(0 < archive.stat().st_size <= 256 * 1024 * 1024, "VST3 ZIP size is invalid")
    evidence = load_json(directory / evidence_name)
    expected_fields = {
        "schemaVersion": 1,
        "artifactStatus": "SIGNED-NOTARIZED",
        "product": product,
        "version": version,
        "tag": f"v{version}",
        "commit": commit,
        "dirty": False,
        "applicationSigned": True,
        "installerSigned": True,
        "notarized": True,
        "applicationSignerSha256": app_pin,
        "installerSignerSha256": pkg_pin,
        "packageFile": package_name,
        "vst3ZipFile": archive_name,
    }
    for key, expected_value in expected_fields.items():
        require(evidence.get(key) == expected_value, f"macOS evidence field is invalid: {key}")
    require(evidence.get("packageSha256") == sha256(package), "macOS PKG hash mismatch")
    require(evidence.get("vst3ZipSha256") == sha256(archive), "macOS VST3 ZIP hash mismatch")
    for key in ("sourceSha256", "pipelineManifestSha256", "sourceManifestSha256"):
        require(SHA256_RE.fullmatch(str(evidence.get(key, ""))) is not None,
                f"macOS evidence hash is invalid: {key}")
    verification = evidence.get("verification")
    require(isinstance(verification, list) and REQUIRED_VERIFICATION.issubset(set(verification)),
            "macOS verification evidence is incomplete")
    try:
        uuid.UUID(str(evidence.get("notarySubmissionId")))
    except ValueError as error:
        raise ContractError("macOS evidence lacks a valid notarization submission ID") from error


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    for command in ("prepare", "validate"):
        sub = subparsers.add_parser(command)
        sub.add_argument("--directory" if command == "validate" else "--candidate", required=True,
                         type=pathlib.Path)
        if command == "prepare":
            sub.add_argument("--output", required=True, type=pathlib.Path)
        sub.add_argument("--product", required=True)
        sub.add_argument("--version", required=True)
        sub.add_argument("--commit", required=True)
        sub.add_argument("--application-signer-sha256", required=True)
        sub.add_argument("--installer-signer-sha256", required=True)
    args = parser.parse_args()
    try:
        common = (
            args.product,
            args.version,
            args.commit,
            args.application_signer_sha256,
            args.installer_signer_sha256,
        )
        if args.command == "prepare":
            prepare(args.candidate, args.output, *common)
        else:
            validate(args.directory, *common)
    except (ContractError, OSError, KeyError, TypeError) as error:
        print(f"macOS release asset validation failed: {error}", file=sys.stderr)
        return 1
    print(f"macOS {args.command} contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
