#!/usr/bin/env python3
"""Fail closed unless a repository has no published signed-Windows baseline."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any


PRODUCT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
VERSION = r"(?:0|[1-9][0-9]{0,2})\.(?:0|[1-9][0-9]{0,2})\.(?:0|[1-9][0-9]{0,4})"
TAG_RE = re.compile(rf"^v{VERSION}$")
MAXIMUM_METADATA_BYTES = 8 * 1024 * 1024


class BootstrapError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BootstrapError(message)


def read_json(path: pathlib.Path, description: str) -> Any:
    require(path.is_file() and not path.is_symlink(), f"{description} is missing or linked")
    require(path.stat().st_size <= MAXIMUM_METADATA_BYTES, f"{description} is unexpectedly large")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BootstrapError(f"{description} is not valid UTF-8 JSON: {error}") from error


def load_releases(path: pathlib.Path, paginated: bool = False) -> list[Any]:
    value = read_json(path, "Release metadata")
    require(isinstance(value, list), "Release metadata root is not an array")
    if not paginated:
        # A single complete REST page means older releases may have been
        # omitted. This mode is retained for local fixtures and fails closed.
        require(len(value) < 100,
                "Cannot prove the complete public release history for bootstrap")
        return value

    # `gh api --paginate --slurp` produces one outer array containing every
    # REST response page. Bound and structurally validate that representation.
    require(0 < len(value) <= 100, "Paginated release metadata has an invalid page count")
    releases: list[Any] = []
    for index, page in enumerate(value):
        require(isinstance(page, list), "Paginated release metadata contains a non-array page")
        require(len(page) <= 100, "Paginated release metadata contains an oversized page")
        if index < len(value) - 1:
            require(len(page) == 100, "Paginated release metadata contains a short intermediate page")
        releases.extend(page)
    release_ids: list[int] = []
    for release in releases:
        require(isinstance(release, dict), "Release metadata contains a non-object")
        release_id = release.get("id")
        require(isinstance(release_id, int) and not isinstance(release_id, bool) and release_id > 0,
                "Paginated release metadata contains an invalid release ID")
        release_ids.append(release_id)
    require(len(release_ids) == len(set(release_ids)),
            "Paginated release metadata contains duplicate release IDs")
    return releases


def verify_bootstrap_policy(policy: Any, product: str, tag: str) -> None:
    require(PRODUCT_RE.fullmatch(product) is not None, "Product name is unsafe")
    require(TAG_RE.fullmatch(tag) is not None, "Bootstrap tag is not canonical")
    require(isinstance(policy, dict), "Bootstrap policy root is not an object")
    require(set(policy) == {"schemaVersion", "product", "bootstrapTag"},
            "Bootstrap policy fields are not exact")
    require(policy["schemaVersion"] == 1, "Bootstrap policy schema is unsupported")
    require(policy["product"] == product, "Bootstrap policy product mismatch")
    require(policy["bootstrapTag"] == tag,
            "This workflow is permanently limited to its declared first Windows release tag")


def verify_no_windows_baseline(
    releases: list[Any],
    product: str,
    allowed_release_id: int | None = None,
    allowed_tag: str | None = None,
) -> None:
    require(PRODUCT_RE.fullmatch(product) is not None, "Product name is unsafe")
    if allowed_release_id is not None:
        require(allowed_release_id > 0, "Allowed release ID is invalid")
        require(allowed_tag is not None and TAG_RE.fullmatch(allowed_tag) is not None,
                "Allowed release tag is invalid")
    asset_pattern = re.compile(
        rf"^{re.escape(product)}-{VERSION}-Windows-(?:x64|arm64ec)\.(?:msi|evidence\.json)$"
    )
    existing: list[str] = []
    allowed_matches = 0
    for release in releases:
        require(isinstance(release, dict), "Release metadata contains a non-object")
        draft = release.get("draft")
        prerelease = release.get("prerelease")
        require(isinstance(draft, bool) and isinstance(prerelease, bool),
                "Release visibility metadata is malformed")
        assets = release.get("assets")
        require(isinstance(assets, list), "Release asset metadata is malformed")
        if allowed_release_id is not None and release.get("id") == allowed_release_id:
            require(not draft and not prerelease,
                    "Allowed release is not a stable published release")
            require(release.get("tag_name") == allowed_tag,
                    "Allowed release identity does not match the bootstrap tag")
            allowed_matches += 1
            continue
        if draft or prerelease:
            continue
        for asset in assets:
            require(isinstance(asset, dict) and isinstance(asset.get("name"), str),
                    "Release contains malformed asset metadata")
            if asset_pattern.fullmatch(asset["name"]):
                existing.append(asset["name"])
    if allowed_release_id is not None:
        require(allowed_matches == 1,
                "Allowed release ID was not found exactly once in public metadata")
    require(
        not existing,
        "A published Windows baseline already exists; exact N-to-N+1 updater acceptance is required: "
        + ", ".join(sorted(existing)),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--releases-json", required=True, type=pathlib.Path)
    parser.add_argument("--policy-json", required=True, type=pathlib.Path)
    parser.add_argument("--product", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--allow-release-id", type=int)
    parser.add_argument("--paginated", action="store_true")
    arguments = parser.parse_args()
    try:
        verify_bootstrap_policy(
            read_json(arguments.policy_json, "Bootstrap policy"), arguments.product, arguments.tag
        )
        verify_no_windows_baseline(
            load_releases(arguments.releases_json, arguments.paginated),
            arguments.product,
            arguments.allow_release_id,
            arguments.tag if arguments.allow_release_id is not None else None,
        )
    except BootstrapError as error:
        print(f"Bootstrap gate failed: {error}", file=sys.stderr)
        return 2
    print("Bootstrap gate passed: no published Windows baseline exists")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
