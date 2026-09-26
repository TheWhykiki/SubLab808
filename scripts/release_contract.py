#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Whykiki Audio
"""Product-neutral release configuration and cross-repository parity contract."""
import argparse
import json
from pathlib import Path
import re
import sys

JUCE_COMMIT = "91ad83ae34a81e0833b1a2b0866f54846370ae53"
JUCE_VERSION = "8.0.15"
NORMALIZED_FILES = {".github/workflows/windows-release.yml", "scripts/test-windows-updater-transition.ps1"}


class ContractError(ValueError):
    pass


def strict_json(raw):
    def pairs(values):
        result = {}
        for key, value in values:
            if key in result:
                raise ContractError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result
    try:
        return json.loads(raw, object_pairs_hook=pairs)
    except (ValueError, UnicodeError) as error:
        raise ContractError(f"Invalid release JSON: {error}") from error


def validate_product(config):
    keys = {"release-contract-version", "productName", "manufacturer", "bundleId", "version",
            "repository", "pluginManufacturerCode", "pluginCode", "license", "juce",
            "vst3Classes", "upgradeCodes"}
    if not isinstance(config, dict) or set(config) != keys:
        raise ContractError("Missing or unexpected product configuration fields")
    if type(config["release-contract-version"]) is not int or config["release-contract-version"] != 2:
        raise ContractError("Unsupported release-contract-version")
    patterns = {"productName": r"[A-Za-z][A-Za-z0-9]{0,63}",
                "manufacturer": r"[A-Za-z][A-Za-z0-9 .-]{0,99}",
                "bundleId": r"[a-z][a-z0-9]*(?:\.[a-z][a-z0-9]*)+",
                "version": r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)",
                "repository": r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+",
                "pluginManufacturerCode": r"[A-Za-z0-9]{4}", "pluginCode": r"[A-Za-z0-9]{4}"}
    for key, pattern in patterns.items():
        if not isinstance(config[key], str) or not re.fullmatch(pattern, config[key]):
            raise ContractError(f"Invalid {key}")
    if config["repository"] != "TheWhykiki/" + config["productName"]:
        raise ContractError("Product repository does not match the canonical owner/product")
    if config["license"] != "AGPL-3.0-only":
        raise ContractError("Own-source license must be AGPL-3.0-only")
    if config["juce"] != {"version": JUCE_VERSION, "commit": JUCE_COMMIT,
                           "sourcePath": "external/JUCE"}:
        raise ContractError("JUCE must use the reviewed version, commit and source location")
    codes = config["upgradeCodes"]
    if not isinstance(codes, dict) or set(codes) != {"x64", "arm64ec"}:
        raise ContractError("Both Windows architecture UpgradeCodes are required")
    if any(not isinstance(code, str) or not re.fullmatch(
            r"[0-9A-F]{8}(?:-[0-9A-F]{4}){3}-[0-9A-F]{12}", code) for code in codes.values()):
        raise ContractError("UpgradeCodes must be canonical uppercase GUIDs")
    if codes["x64"] == codes["arm64ec"]:
        raise ContractError("Architecture UpgradeCodes must differ")
    classes = config["vst3Classes"]
    categories = {"Audio Module Class", "Component Controller Class"}
    if not isinstance(classes, list) or len(classes) != 2 or any(
            not isinstance(item, dict) or set(item) != {"cid", "category"}
            or not isinstance(item["cid"], str) or not re.fullmatch(r"[0-9A-F]{32}", item["cid"])
            or item["category"] not in categories for item in classes):
        raise ContractError("Exactly two canonical VST3 classes are required")
    if {item["category"] for item in classes} != categories or len({item["cid"] for item in classes}) != 2:
        raise ContractError("VST3 class identities must be distinct")
    return config


def load_product(root):
    return validate_product(strict_json((Path(root) / "release/product.json").read_bytes()))


def verify_product_projection(root):
    root = Path(root)
    config = load_product(root)
    installer = strict_json((root / "Installer/Windows/package-config.json").read_bytes())
    expected = {"schemaVersion": 1, **{key: config[key] for key in
                ("productName", "manufacturer", "vst3Classes", "upgradeCodes")}}
    if installer != expected:
        raise ContractError("Legacy installer configuration differs from release/product.json")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    # Both the existing literal CMake and the shared CMake configuration are
    # checked during migration. Runtime CMake asserts its own product identity.
    literal = re.search(r"project\(\s*" + re.escape(config["productName"])
                        + r"\s+VERSION\s+(\d+\.\d+\.\d+)", cmake)
    if literal:
        if literal.group(1) != config["version"]:
            raise ContractError("CMake version differs from release/product.json")
    elif "cmake/ReleaseProduct.cmake" not in cmake:
        raise ContractError("CMake must consume the shared release product configuration")
    return config


def parity_files(root):
    value = strict_json((Path(root) / "release/shared-files.json").read_bytes())
    if not isinstance(value, dict) or set(value) != {"release-contract-version", "files", "productNormalizedFiles"} \
            or value["release-contract-version"] != 2 or not isinstance(value["files"], list) \
            or not isinstance(value["productNormalizedFiles"], list):
        raise ContractError("Invalid shared-file parity manifest")
    normalized = value["productNormalizedFiles"]
    for group in (value["files"], normalized):
        if any(not isinstance(name, str) for name in group) or group != sorted(set(group)):
            raise ContractError("Shared files must be sorted unique lists of paths")
    if not set(normalized) <= NORMALIZED_FILES:
        raise ContractError("Unexpected product-normalized release file")
    if set(normalized) & set(value["files"]):
        raise ContractError("Shared-file parity modes overlap")
    files = value["files"] + normalized
    if not files:
        raise ContractError("Shared files cannot be empty")
    files = sorted(files)
    for name in files:
        if not isinstance(name, str) or name.startswith("/") or "\\" in name \
                or any(part in {"", ".", ".."} for part in name.split("/")):
            raise ContractError("Unsafe shared-file path")
        path = Path(root) / name
        if path.is_symlink() or not path.is_file():
            raise ContractError(f"Missing regular shared release file: {name}")
    return files


def normalize_product_file(raw, name, config):
    """Only declared identity substitutions; every guard/command/byte survives."""
    substitutions = [(config["productName"], "@@WK_PRODUCT_NAME@@")]
    if name == ".github/workflows/windows-release.yml":
        substitutions += [(config["productName"].upper(), "@@WK_PRODUCT_UPPER@@"),
                          (config["upgradeCodes"]["x64"], "@@WK_UPGRADE_X64@@"),
                          (config["upgradeCodes"]["arm64ec"], "@@WK_UPGRADE_ARM64EC@@")]
    elif name != "scripts/test-windows-updater-transition.ps1":
        raise ContractError("No normalization policy for this release file")
    value = raw.decode("utf-8")
    if any(marker in value for _, marker in substitutions):
        raise ContractError("Reserved normalization marker in release implementation")
    for literal, marker in substitutions:
        value = value.replace(literal, marker)
    return value.encode("utf-8")


def verify_parity(root, other):
    config = verify_product_projection(root)
    peer_config = verify_product_projection(other)
    files = parity_files(root)
    if files != parity_files(other):
        raise ContractError("The repositories disagree about which files must remain shared")
    normalized = strict_json((Path(root) / "release/shared-files.json").read_bytes())["productNormalizedFiles"]
    peer_normalized = strict_json((Path(other) / "release/shared-files.json").read_bytes())["productNormalizedFiles"]
    if normalized != peer_normalized:
        raise ContractError("The repositories disagree about product-normalized parity files")
    changed = []
    for name in files:
        current, peer = (Path(root) / name).read_bytes(), (Path(other) / name).read_bytes()
        if name in normalized:
            current = normalize_product_file(current, name, config)
            peer = normalize_product_file(peer, name, peer_config)
        if current != peer:
            changed.append(name)
    if changed:
        raise ContractError("Shared release implementation drift: " + ", ".join(changed))
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--other", type=Path)
    options = parser.parse_args()
    try:
        verify_product_projection(options.repo)
        count = len(verify_parity(options.repo, options.other) if options.other else parity_files(options.repo))
    except (ContractError, OSError) as error:
        parser.exit(1, f"Release contract rejected: {error}\n")
    print(f"Release contract v2 verified; {count} shared files" + (" match declared exact/product-identity policies" if options.other else " present"))


if __name__ == "__main__":
    main()
