#!/usr/bin/env python3
"""Resolve the immutable Windows release-chain state for a candidate tag.

The input is the exact JSON shape emitted by ``gh api --paginate --slurp``.
Only a first-release bootstrap, a strict upgrade from the newest complete
Windows release, a retry above immutable quarantined prereleases, or a
post-promotion recheck of one explicitly identified candidate can succeed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


OWNER = "TheWhykiki"
PAGE_SIZE = 100
MAXIMUM_PAGES = 100
MAXIMUM_METADATA_BYTES = 8 * 1024 * 1024
PRODUCT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$")
VERSION_RE = re.compile(
    r"^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$"
)
TAG_RE = re.compile(
    r"^v(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$"
)
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")


class ReleaseStateError(RuntimeError):
    """Raised when public release history cannot be resolved unambiguously."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ReleaseStateError(message)


def _is_positive_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def _is_safe_text(value: Any, maximum_length: int) -> bool:
    return (isinstance(value, str) and 0 < len(value) <= maximum_length
            and all(ord(character) >= 32 and ord(character) != 127 for character in value))


def _unique_json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        require(key not in result, f"JSON object contains duplicate key {key!r}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> None:
    raise ReleaseStateError(f"JSON contains non-finite number {value}")


def read_json(path: pathlib.Path, description: str) -> Any:
    require(path.is_file() and not path.is_symlink(), f"{description} is missing or linked")
    size = path.stat().st_size
    require(0 < size <= MAXIMUM_METADATA_BYTES, f"{description} has an invalid size")
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_unique_json_object,
            parse_constant=_reject_json_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ReleaseStateError(f"{description} is not valid UTF-8 JSON: {error}") from error


def load_paginated_releases(path: pathlib.Path) -> list[Any]:
    """Load the complete, slurped array-of-pages representation fail-closed."""

    pages = read_json(path, "Release metadata")
    require(isinstance(pages, list), "Slurped release metadata root is not an array")
    require(0 < len(pages) <= MAXIMUM_PAGES,
            "Slurped release metadata has an invalid page count")
    releases: list[Any] = []
    for index, page in enumerate(pages):
        require(isinstance(page, list), "Slurped release metadata contains a non-array page")
        require(len(page) <= PAGE_SIZE, "Slurped release metadata contains an oversized page")
        if index < len(pages) - 1:
            require(len(page) == PAGE_SIZE,
                    "Slurped release metadata contains a short intermediate page")
        releases.extend(page)
    return releases


def parse_tag(tag: Any, description: str) -> tuple[int, int, int]:
    require(isinstance(tag, str), f"{description} is not a string")
    match = TAG_RE.fullmatch(tag)
    require(match is not None, f"{description} is not canonical")
    version = tuple(int(part) for part in match.groups())
    require(version[0] <= 255 and version[1] <= 255 and version[2] <= 65535,
            f"{description} exceeds Windows Installer version bounds")
    return version


def version_text(version: tuple[int, int, int]) -> str:
    return ".".join(str(part) for part in version)


def validate_policy(policy: Any, product: str) -> tuple[int, int, int]:
    require(isinstance(product, str) and PRODUCT_RE.fullmatch(product) is not None,
            "Product name is unsafe")
    require(isinstance(policy, dict), "Bootstrap policy root is not an object")
    require(set(policy) == {"schemaVersion", "product", "bootstrapTag"},
            "Bootstrap policy fields are not exact")
    require(policy["schemaVersion"] == 1 and not isinstance(policy["schemaVersion"], bool),
            "Bootstrap policy schema is unsupported")
    require(policy["product"] == product, "Bootstrap policy product mismatch")
    return parse_tag(policy["bootstrapTag"], "Bootstrap policy tag")


def parse_confirmation(value: str) -> bool:
    require(value in ("true", "false"), "Confirmation must be exactly true or false")
    return value == "true"


def expected_release_asset_names(product: str, version: str) -> tuple[str, ...]:
    windows = tuple(
        f"{product}-{version}-Windows-{architecture}.{suffix}"
        for architecture in ("x64", "arm64ec")
        for suffix in ("msi", "evidence.json")
    )
    return windows + (
        f"{product}-{version}-macOS-universal.pkg",
        f"{product}-{version}-macOS-universal-VST3.zip",
        f"{product}-{version}-macOS-universal.evidence.json",
        f"{product}-{version}-SHA256SUMS.txt",
    )


def expected_release_url(product: str, tag: str) -> str:
    return f"https://github.com/{OWNER}/{product}/releases/tag/{tag}"


def expected_asset_url(product: str, tag: str, name: str) -> str:
    return f"https://github.com/{OWNER}/{product}/releases/download/{tag}/{name}"


class NormalizedRelease:
    __slots__ = (
        "release_id", "tag", "version", "target_commitish", "draft",
        "prerelease", "immutable", "html_url", "assets", "is_windows_release",
    )

    def __init__(
        self,
        release_id: int,
        tag: str,
        version: tuple[int, int, int] | None,
        target_commitish: str,
        draft: bool,
        prerelease: bool,
        immutable: bool,
        html_url: str,
        assets: tuple[dict[str, Any], ...],
        is_windows_release: bool,
    ) -> None:
        self.release_id = release_id
        self.tag = tag
        self.version = version
        self.target_commitish = target_commitish
        self.draft = draft
        self.prerelease = prerelease
        self.immutable = immutable
        self.html_url = html_url
        self.assets = assets
        self.is_windows_release = is_windows_release

    def history_value(self) -> dict[str, Any]:
        return {
            "assets": list(self.assets),
            "draft": self.draft,
            "htmlUrl": self.html_url,
            "id": self.release_id,
            "immutable": self.immutable,
            "prerelease": self.prerelease,
            "tag": self.tag,
            "targetCommitish": self.target_commitish,
        }


def _normalize_releases(releases: list[Any], product: str) -> list[NormalizedRelease]:
    seen_release_ids: set[int] = set()
    seen_tags: set[str] = set()
    seen_asset_ids: set[int] = set()
    seen_asset_urls: set[str] = set()
    normalized: list[NormalizedRelease] = []
    product_prefix = f"{product}-".casefold()
    product_windows_pattern = re.compile(
        rf"^{re.escape(product)}-"
        rf"((?:0|[1-9][0-9]{{0,2}})\.(?:0|[1-9][0-9]{{0,2}})\."
        rf"(?:0|[1-9][0-9]{{0,4}}))-Windows-(x64|arm64ec)\."
        rf"(msi|evidence\.json)$"
    )

    for release_index, raw_release in enumerate(releases):
        description = f"Release record {release_index}"
        require(isinstance(raw_release, dict), f"{description} is not an object")
        release_id = raw_release.get("id")
        require(_is_positive_int(release_id), f"{description} has an invalid ID")
        require(release_id not in seen_release_ids, "Release metadata contains duplicate IDs")
        seen_release_ids.add(release_id)

        tag = raw_release.get("tag_name")
        require(_is_safe_text(tag, 200),
                f"{description} tag identity is malformed")
        require(tag not in seen_tags, "Release metadata contains duplicate tags")
        seen_tags.add(tag)
        version_match = TAG_RE.fullmatch(tag)
        version = (tuple(int(part) for part in version_match.groups())
                   if version_match is not None else None)

        draft = raw_release.get("draft")
        prerelease = raw_release.get("prerelease")
        immutable = raw_release.get("immutable")
        require(isinstance(draft, bool) and isinstance(prerelease, bool)
                    and isinstance(immutable, bool),
                f"{description} publication flags are malformed")

        target = raw_release.get("target_commitish")
        require(_is_safe_text(target, 200),
                f"{description} target commitish is malformed")
        html_url = raw_release.get("html_url")
        require(_is_safe_text(html_url, 4096), f"{description} URL is malformed")
        raw_assets = raw_release.get("assets")
        require(isinstance(raw_assets, list), f"{description} assets are not an array")

        asset_names: set[str] = set()
        asset_urls: set[str] = set()
        windows_names: list[str] = []
        normalized_assets: list[dict[str, Any]] = []
        for asset_index, raw_asset in enumerate(raw_assets):
            asset_description = f"{description} asset {asset_index}"
            require(isinstance(raw_asset, dict), f"{asset_description} is not an object")
            asset_id = raw_asset.get("id")
            require(_is_positive_int(asset_id), f"{asset_description} has an invalid ID")
            require(asset_id not in seen_asset_ids, "Release metadata contains duplicate asset IDs")
            seen_asset_ids.add(asset_id)
            name = raw_asset.get("name")
            require(_is_safe_text(name, 255),
                    f"{asset_description} has an unsafe name")
            require(name not in asset_names, f"{description} contains duplicate asset names")
            asset_names.add(name)
            state = raw_asset.get("state")
            require(_is_safe_text(state, 32),
                    f"{asset_description} has a malformed state")
            require("digest" in raw_asset, f"{asset_description} has no digest field")
            digest = raw_asset.get("digest")
            require(digest is None or _is_safe_text(digest, 200),
                    f"{asset_description} has a malformed digest")
            size = raw_asset.get("size")
            require(isinstance(size, int) and not isinstance(size, bool) and size >= 0,
                    f"{asset_description} has an invalid size")
            url = raw_asset.get("browser_download_url")
            require(_is_safe_text(url, 4096), f"{asset_description} URL is malformed")
            require(url not in asset_urls, f"{description} contains duplicate asset URLs")
            require(url not in seen_asset_urls,
                    "Release metadata contains duplicate asset URLs")
            asset_urls.add(url)
            seen_asset_urls.add(url)
            normalized_assets.append(
                {"digest": digest, "id": asset_id, "name": name,
                 "size": size, "state": state, "url": url}
            )

            folded_name = name.casefold()
            if folded_name.startswith(product_prefix) and "windows" in folded_name:
                match = product_windows_pattern.fullmatch(name)
                require(match is not None, f"{asset_description} is a malformed Windows asset")
                parsed_release_version = parse_tag(tag, f"{description} Windows tag")
                expected_version = version_text(parsed_release_version)
                asset_version = VERSION_RE.fullmatch(match.group(1))
                require(asset_version is not None, f"{asset_description} version is malformed")
                parsed_asset_version = tuple(int(part) for part in asset_version.groups())
                require(parsed_asset_version[0] <= 255 and parsed_asset_version[1] <= 255
                            and parsed_asset_version[2] <= 65535,
                        f"{asset_description} version exceeds Windows Installer bounds")
                require(match.group(1) == expected_version,
                        f"{asset_description} version does not match its release tag")
                require(state == "uploaded", f"{asset_description} is not fully uploaded")
                require(isinstance(digest, str) and DIGEST_RE.fullmatch(digest) is not None,
                        f"{asset_description} has no canonical SHA-256 digest")
                require(_is_positive_int(size), f"{asset_description} has an invalid size")
                require(url == expected_asset_url(product, tag, name),
                        f"{asset_description} URL is not canonical")
                windows_names.append(name)
                version = parsed_release_version

        is_windows_release = bool(windows_names)
        if is_windows_release:
            require(version is not None, f"Windows release {tag} has no canonical version")
            expected_version = version_text(version)
            expected_names = set(expected_release_asset_names(product, expected_version))
            require(len(asset_names) == len(expected_names) and asset_names == expected_names,
                    f"Windows release {tag} does not contain the exact cross-platform asset set")
            for release_asset in normalized_assets:
                require(release_asset["state"] == "uploaded",
                        f"Windows release {tag} asset {release_asset['name']} is not fully uploaded")
                require(isinstance(release_asset["digest"], str)
                            and DIGEST_RE.fullmatch(release_asset["digest"]) is not None,
                        f"Windows release {tag} asset {release_asset['name']} has no canonical SHA-256 digest")
                require(_is_positive_int(release_asset["size"]),
                        f"Windows release {tag} asset {release_asset['name']} has an invalid size")
                require(release_asset["url"]
                            == expected_asset_url(product, tag, release_asset["name"]),
                        f"Windows release {tag} asset {release_asset['name']} URL is not canonical")
            require(COMMIT_RE.fullmatch(target) is not None,
                    f"Windows release {tag} is not bound to an exact commit")
            require(html_url == expected_release_url(product, tag),
                    f"Windows release {tag} URL is not canonical")
            require(not draft, f"Draft Windows release {tag} is not a valid chain entry")
            require(immutable, f"Windows release {tag} is not immutable")

        normalized.append(
            NormalizedRelease(
                release_id=release_id,
                tag=tag,
                version=version,
                target_commitish=target,
                draft=draft,
                prerelease=prerelease,
                immutable=immutable,
                html_url=html_url,
                assets=tuple(sorted(normalized_assets, key=lambda asset: (asset["name"], asset["id"]))),
                is_windows_release=is_windows_release,
            )
        )
    return normalized


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def _history_digest(product: str, releases: list[NormalizedRelease]) -> str:
    history = {
        "product": product,
        "releases": [release.history_value()
                     for release in sorted(releases, key=lambda item: item.release_id)],
        "schemaVersion": 1,
    }
    return "sha256:" + hashlib.sha256(_canonical_bytes(history)).hexdigest()


def resolve_release_state(
    releases: list[Any],
    policy: Any,
    product: str,
    candidate_tag: str,
    confirmation: str = "false",
    allow_candidate_release_id: int | None = None,
) -> dict[str, Any]:
    bootstrap_version = validate_policy(policy, product)
    candidate_version = parse_tag(candidate_tag, "Candidate tag")
    confirmed = parse_confirmation(confirmation)
    if allow_candidate_release_id is not None:
        require(_is_positive_int(allow_candidate_release_id),
                "Allowed candidate release ID is invalid")

    normalized = _normalize_releases(releases, product)
    candidate_records = [release for release in normalized if release.tag == candidate_tag]
    allowed_candidate: NormalizedRelease | None = None
    if allow_candidate_release_id is None:
        require(not candidate_records,
                "Candidate tag already has a release; only its exact post-promotion ID may be allowed")
    else:
        matching_ids = [release for release in normalized
                        if release.release_id == allow_candidate_release_id]
        require(len(matching_ids) == 1, "Allowed candidate release ID was not found exactly once")
        allowed_candidate = matching_ids[0]
        require(allowed_candidate.tag == candidate_tag,
                "Allowed candidate release ID does not match the candidate tag")
        require(not allowed_candidate.draft and not allowed_candidate.prerelease
                    and allowed_candidate.is_windows_release,
                "Allowed candidate release is not the complete stable Windows candidate")
        require(len(candidate_records) == 1 and candidate_records[0] == allowed_candidate,
                "Candidate release identity is ambiguous")

    history = [release for release in normalized if release != allowed_candidate]
    baselines = [release for release in history
                 if release.is_windows_release and not release.draft and not release.prerelease]
    quarantines = [release for release in history
                   if release.is_windows_release and not release.draft and release.prerelease]
    windows_chain = baselines + quarantines
    if windows_chain:
        newest_chain_entry = max(
            windows_chain,
            key=lambda release: release.version if release.version is not None else (-1, -1, -1),
        )
        require(newest_chain_entry.version is not None
                    and candidate_version > newest_chain_entry.version,
                "Candidate tag is not strictly newer than every stable or quarantined "
                "Windows release")

    if baselines:
        baseline = max(
            baselines,
            key=lambda release: release.version if release.version is not None else (-1, -1, -1),
        )
        require(baseline.version is not None, "Stable Windows baseline has no version")
        mode = "upgrade"
        baseline_value: dict[str, Any] | None = {
            "commit": baseline.target_commitish,
            "releaseId": baseline.release_id,
            "tag": baseline.tag,
            "version": version_text(baseline.version),
        }
    elif quarantines:
        # There is still no installable N release, but a failed immutable bootstrap
        # attempt reserves its version.  Continue in bootstrap mode above it without
        # replaying the one-time empty-history confirmation.
        mode = "bootstrap"
        baseline_value = None
    else:
        require(candidate_version == bootstrap_version,
                "An empty Windows history is allowed only for the exact bootstrap-policy tag")
        require(confirmed, "First Windows release bootstrap requires explicit confirmation")
        mode = "bootstrap"
        baseline_value = None

    return {
        "baseline": baseline_value,
        "candidate": {"tag": candidate_tag, "version": version_text(candidate_version)},
        "historyDigest": _history_digest(product, history),
        "mode": mode,
        "product": product,
        "schemaVersion": 1,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--releases-json", required=True, type=pathlib.Path)
    parser.add_argument("--policy-json", required=True, type=pathlib.Path)
    parser.add_argument("--product", required=True)
    parser.add_argument("--candidate-tag", required=True)
    parser.add_argument("--confirmation", choices=("true", "false"), default="false")
    parser.add_argument("--allow-candidate-release-id", type=int)
    arguments = parser.parse_args()
    try:
        result = resolve_release_state(
            load_paginated_releases(arguments.releases_json),
            read_json(arguments.policy_json, "Bootstrap policy"),
            arguments.product,
            arguments.candidate_tag,
            arguments.confirmation,
            arguments.allow_candidate_release_id,
        )
    except (OSError, ReleaseStateError) as error:
        print(f"Windows release-state gate failed: {error}", file=sys.stderr)
        return 2
    sys.stdout.buffer.write(_canonical_bytes(result) + b"\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
