#!/usr/bin/env python3
"""Validate a protected-environment review as an exact physical DAW receipt."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import re
from typing import Any


class ContractError(ValueError):
    """The physical acceptance receipt does not satisfy the release contract."""


SHA256 = re.compile(r"^sha256:[0-9a-f]{64}$")
HEX_SHA256 = re.compile(r"^[0-9a-f]{64}$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
TAG = re.compile(r"^v(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,4})$")
LOGIN = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$")
RFC3339_SECONDS = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
DETAIL = re.compile(r"^[^\x00-\x1f\x7f]{1,160}$")


def _error(message: str) -> None:
    raise ContractError(message)


def _object_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            _error(f"JSON contains duplicate key: {key}")
        value[key] = item
    return value


def _parse_json(text: str, label: str) -> Any:
    try:
        return json.loads(text, object_pairs_hook=_object_pairs)
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise ContractError(f"{label} is not valid JSON: {exc}") from exc


def load_json(path: pathlib.Path, label: str) -> Any:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ContractError(f"Cannot read {label}: {exc}") from exc
    if len(data) > 2 * 1024 * 1024:
        _error(f"{label} exceeds the fixed size bound")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ContractError(f"{label} is not UTF-8: {exc}") from exc
    return _parse_json(text, label)


def _require_object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _error(f"{label} must be an object")
    return value


def _require_exact_keys(value: dict[str, Any], keys: set[str], label: str) -> None:
    if set(value) != keys:
        _error(f"{label} fields are not exact")


def _require_positive_int(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        _error(f"{label} must be a positive integer")
    return value


def _parse_time(value: Any, label: str) -> dt.datetime:
    if not isinstance(value, str) or not RFC3339_SECONDS.fullmatch(value):
        _error(f"{label} must be UTC RFC3339 with whole seconds")
    try:
        return dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(
            tzinfo=dt.timezone.utc
        )
    except ValueError as exc:
        raise ContractError(f"{label} is not a real timestamp") from exc


def _manifest_sha256(assets: list[dict[str, Any]]) -> str:
    manifest = [
        {
            "digest": asset["digest"],
            "id": asset["id"],
            "name": asset["name"],
            "size": asset["size"],
            "state": asset["state"],
        }
        for asset in sorted(assets, key=lambda item: (item["name"], item["id"]))
    ]
    encoded = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode(
        "utf-8"
    )
    return hashlib.sha256(encoded).hexdigest()


def _require_ancestor(value: Any, *, ancestor: str, label: str) -> None:
    comparison = _require_object(value, label)
    _require_exact_keys(
        comparison,
        {"status", "base_commit", "merge_base_commit"},
        label,
    )
    base_commit = _require_object(comparison.get("base_commit"), f"{label} base")
    merge_base = _require_object(
        comparison.get("merge_base_commit"), f"{label} merge base"
    )
    for commit_object, commit_label in (
        (base_commit, "base"),
        (merge_base, "merge base"),
    ):
        _require_exact_keys(commit_object, {"sha"}, f"{label} {commit_label}")
    if (
        comparison.get("status") not in {"ahead", "identical"}
        or base_commit.get("sha") != ancestor
        or merge_base.get("sha") != ancestor
    ):
        _error(f"{label} does not prove the required ancestry")


def validate(
    *,
    environment: Any,
    reviews: Any,
    workflow_run: Any,
    branch: Any,
    candidate_compare: Any,
    workflow_compare: Any,
    release: Any,
    repository: str,
    product: str,
    environment_name: str,
    run_id: int,
    run_attempt: int,
    workflow_sha: str,
    release_id: int,
    tag: str,
    commit: str,
    asset_manifest_sha256: str,
    now: dt.datetime | None = None,
) -> dict[str, Any]:
    if (
        not TAG.fullmatch(tag)
        or not COMMIT.fullmatch(commit)
        or not COMMIT.fullmatch(workflow_sha)
    ):
        _error("Expected tag, candidate commit, or workflow commit is malformed")
    if not HEX_SHA256.fullmatch(asset_manifest_sha256):
        _error("Expected asset-manifest SHA-256 is malformed")
    _require_positive_int(run_id, "runId")
    _require_positive_int(run_attempt, "runAttempt")
    _require_positive_int(release_id, "releaseId")

    environment = _require_object(environment, "Environment response")
    environment_id = _require_positive_int(environment.get("id"), "environment.id")
    if environment.get("name") != environment_name:
        _error("Environment response names a different environment")
    rules = environment.get("protection_rules")
    if not isinstance(rules, list):
        _error("Environment protection rules are missing")
    reviewer_rules = [
        rule
        for rule in rules
        if isinstance(rule, dict) and rule.get("type") == "required_reviewers"
    ]
    if len(reviewer_rules) != 1:
        _error("Environment must have exactly one required-reviewers rule")
    reviewer_rule = reviewer_rules[0]
    if reviewer_rule.get("prevent_self_review") is not True:
        _error("Environment must prevent self-review")
    deployment_branch_policy = _require_object(
        environment.get("deployment_branch_policy"),
        "Environment deployment-branch policy",
    )
    if (
        deployment_branch_policy.get("protected_branches") is not True
        or deployment_branch_policy.get("custom_branch_policies") is not False
    ):
        _error("Environment must allow only protected branches")
    configured_reviewers = reviewer_rule.get("reviewers")
    if not isinstance(configured_reviewers, list) or not configured_reviewers:
        _error("Environment has no configured required reviewer")
    allowed_reviewers: set[tuple[int, str]] = set()
    for configured in configured_reviewers:
        configured = _require_object(configured, "Configured environment reviewer")
        if configured.get("type") != "User":
            _error("Environment required reviewers must be explicit users, not teams")
        configured_user = _require_object(
            configured.get("reviewer"), "Configured environment reviewer user"
        )
        configured_id = _require_positive_int(
            configured_user.get("id"), "configured reviewer id"
        )
        configured_login = configured_user.get("login")
        if not isinstance(configured_login, str) or not LOGIN.fullmatch(configured_login):
            _error("Configured environment reviewer login is malformed")
        allowed_reviewers.add((configured_id, configured_login.casefold()))

    workflow_run = _require_object(workflow_run, "Workflow-run response")
    run_repository = _require_object(
        workflow_run.get("repository"), "Workflow-run repository"
    )
    default_branch = run_repository.get("default_branch")
    if (
        workflow_run.get("id") != run_id
        or workflow_run.get("run_attempt") != run_attempt
        or workflow_run.get("event") != "workflow_dispatch"
        or workflow_run.get("head_sha") != workflow_sha
        or run_repository.get("full_name") != repository
        or not isinstance(default_branch, str)
        or not default_branch
        or workflow_run.get("head_branch") != default_branch
    ):
        _error("Workflow-run identity does not match the current attempt")

    branch = _require_object(branch, "Default-branch response")
    branch_commit = _require_object(branch.get("commit"), "Default-branch commit")
    observed_branch_commit = branch_commit.get("sha")
    if (
        branch.get("name") != default_branch
        or branch.get("protected") is not True
        or not isinstance(observed_branch_commit, str)
        or not COMMIT.fullmatch(observed_branch_commit)
    ):
        _error("Repository default branch is not currently protected")
    _require_ancestor(
        candidate_compare,
        ancestor=commit,
        label="Candidate/default-branch comparison",
    )
    _require_ancestor(
        workflow_compare,
        ancestor=workflow_sha,
        label="Workflow/default-branch comparison",
    )
    actor_object = _require_object(workflow_run.get("actor"), "Workflow actor")
    triggering_actor_object = _require_object(
        workflow_run.get("triggering_actor"), "Workflow triggering actor"
    )
    actor_id = _require_positive_int(actor_object.get("id"), "workflow actor id")
    triggering_actor_id = _require_positive_int(
        triggering_actor_object.get("id"), "workflow triggering actor id"
    )
    actor = actor_object.get("login")
    triggering_actor = triggering_actor_object.get("login")
    if (
        not isinstance(actor, str)
        or not LOGIN.fullmatch(actor)
        or not isinstance(triggering_actor, str)
        or not LOGIN.fullmatch(triggering_actor)
    ):
        _error("Workflow-run actor identities are malformed")

    if not isinstance(reviews, list) or len(reviews) != 1:
        _error("Workflow run must contain exactly one deployment review")
    review = _require_object(reviews[0], "Deployment review")
    if review.get("state") != "approved":
        _error("Deployment review is not approved")
    reviewed_environments = review.get("environments")
    if not isinstance(reviewed_environments, list) or len(reviewed_environments) != 1:
        _error("Deployment review must cover exactly one environment")
    reviewed_environment = _require_object(
        reviewed_environments[0], "Reviewed environment"
    )
    if (
        reviewed_environment.get("id") != environment_id
        or reviewed_environment.get("name") != environment_name
    ):
        _error("Deployment review covers a different environment")
    review_user = _require_object(review.get("user"), "Deployment review user")
    reviewer_id = _require_positive_int(review_user.get("id"), "deployment reviewer id")
    reviewer = review_user.get("login")
    if not isinstance(reviewer, str) or not LOGIN.fullmatch(reviewer):
        _error("Deployment reviewer identity is malformed")
    if (reviewer_id, reviewer.casefold()) not in allowed_reviewers:
        _error("Deployment reviewer is not an explicitly configured user reviewer")
    if reviewer_id in {actor_id, triggering_actor_id} or reviewer.casefold() in {
        actor.casefold(),
        triggering_actor.casefold(),
    }:
        _error("Workflow initiator or re-run initiator cannot attest acceptance")
    comment = review.get("comment")
    if not isinstance(comment, str) or not comment or len(comment.encode("utf-8")) > 16384:
        _error("Deployment review comment is missing or too large")
    receipt = _require_object(_parse_json(comment, "Deployment review comment"), "Receipt")

    release = _require_object(release, "Release response")
    marker = f"whykiki-release-run:{repository}:{run_id}:{run_attempt}:{commit}"
    release_body = release.get("body")
    if (
        release.get("id") != release_id
        or release.get("tag_name") != tag
        or release.get("target_commitish") != commit
        or release.get("draft") is not False
        or release.get("prerelease") is not True
        or release.get("immutable") is not True
        or not isinstance(release_body, str)
        or marker not in release_body
    ):
        _error("Release is not the exact immutable staged candidate")
    assets_value = release.get("assets")
    if not isinstance(assets_value, list) or len(assets_value) != 8:
        _error("Release must contain exactly eight assets")
    assets: list[dict[str, Any]] = []
    for index, item in enumerate(assets_value):
        asset = _require_object(item, f"release.assets[{index}]")
        name = asset.get("name")
        if (
            not isinstance(name, str)
            or not name
            or not SHA256.fullmatch(str(asset.get("digest", "")))
            or asset.get("state") != "uploaded"
        ):
            _error("Release asset metadata is incomplete")
        _require_positive_int(asset.get("id"), "release asset id")
        _require_positive_int(asset.get("size"), "release asset size")
        assets.append(asset)
    if len({asset["name"] for asset in assets}) != len(assets):
        _error("Release asset names are not unique")
    version = tag[1:]
    expected_asset_names = {
        f"{product}-{version}-Windows-x64.msi",
        f"{product}-{version}-Windows-x64.evidence.json",
        f"{product}-{version}-Windows-arm64ec.msi",
        f"{product}-{version}-Windows-arm64ec.evidence.json",
        f"{product}-{version}-macOS-universal.pkg",
        f"{product}-{version}-macOS-universal-VST3.zip",
        f"{product}-{version}-macOS-universal.evidence.json",
        f"{product}-{version}-SHA256SUMS.txt",
    }
    if {asset["name"] for asset in assets} != expected_asset_names:
        _error("Release asset names do not match the cross-platform contract")
    if _manifest_sha256(assets) != asset_manifest_sha256:
        _error("Release asset manifest does not match the staged digest")

    receipt_keys = {
        "schemaVersion",
        "repository",
        "product",
        "runId",
        "runAttempt",
        "releaseId",
        "tag",
        "commit",
        "assetManifestSha256",
        "artifacts",
        "checks",
    }
    _require_exact_keys(receipt, receipt_keys, "Receipt")
    expected_identity = {
        "schemaVersion": 1,
        "repository": repository,
        "product": product,
        "runId": run_id,
        "runAttempt": run_attempt,
        "releaseId": release_id,
        "tag": tag,
        "commit": commit,
        "assetManifestSha256": asset_manifest_sha256,
    }
    for key, expected in expected_identity.items():
        if receipt.get(key) != expected:
            _error(f"Receipt {key} does not match the staged candidate")

    asset_by_name = {asset["name"]: asset for asset in assets}
    deliverables = {
        f"{product}-{version}-Windows-x64.msi",
        f"{product}-{version}-Windows-arm64ec.msi",
        f"{product}-{version}-macOS-universal.pkg",
        f"{product}-{version}-macOS-universal-VST3.zip",
    }
    artifacts = _require_object(receipt.get("artifacts"), "Receipt artifacts")
    if set(artifacts) != deliverables:
        _error("Receipt must bind exactly the four installable delivery artifacts")
    for name in deliverables:
        if artifacts[name] != asset_by_name[name]["digest"]:
            _error(f"Receipt digest does not match release asset: {name}")

    checks = receipt.get("checks")
    if not isinstance(checks, list) or len(checks) != 8:
        _error("Receipt must contain exactly eight physical DAW checks")
    expected_checks = {
        (platform, host)
        for platform in (
            "windows-x64-msi",
            "windows-arm64ec-msi",
            "macos-universal-pkg",
            "macos-universal-zip",
        )
        for host in ("Cubase", "Reaper")
    }
    actual_checks: set[tuple[str, str]] = set()
    published_at = _parse_time(release.get("published_at"), "release.published_at")
    current_time = now or dt.datetime.now(dt.timezone.utc)
    if current_time.tzinfo is None:
        current_time = current_time.replace(tzinfo=dt.timezone.utc)
    for index, item in enumerate(checks):
        check = _require_object(item, f"Receipt check {index}")
        _require_exact_keys(
            check,
            {
                "platform",
                "host",
                "hostVersion",
                "osVersion",
                "machine",
                "tester",
                "testedAt",
                "result",
            },
            f"Receipt check {index}",
        )
        pair = (check.get("platform"), check.get("host"))
        if pair not in expected_checks or pair in actual_checks:
            _error("Receipt DAW matrix has a missing, duplicate, or unknown entry")
        actual_checks.add(pair)
        if check.get("result") != "pass":
            _error("Every physical DAW check must explicitly pass")
        for field in ("hostVersion", "osVersion", "machine", "tester"):
            value = check.get(field)
            if not isinstance(value, str) or value.strip() != value or not DETAIL.fullmatch(value):
                _error(f"Receipt check {field} is missing or unsafe")
        tested_at = _parse_time(check.get("testedAt"), "Receipt check testedAt")
        if tested_at < published_at or tested_at > current_time + dt.timedelta(minutes=5):
            _error("Physical DAW check timestamp is outside the candidate lifetime")
    if actual_checks != expected_checks:
        _error("Receipt does not cover Cubase and Reaper for every delivery artifact")

    return {
        "schemaVersion": 1,
        "environment": environment_name,
        "environmentId": environment_id,
        "branchProtection": {
            "candidateAncestor": commit,
            "name": default_branch,
            "observedCommit": observed_branch_commit,
            "protected": True,
            "workflowAncestor": workflow_sha,
        },
        "review": {
            "state": "approved",
            "user": reviewer,
            "userId": reviewer_id,
        },
        "receipt": receipt,
    }


def _positive(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--environment-json", type=pathlib.Path, required=True)
    parser.add_argument("--reviews-json", type=pathlib.Path, required=True)
    parser.add_argument("--workflow-run-json", type=pathlib.Path, required=True)
    parser.add_argument("--branch-json", type=pathlib.Path, required=True)
    parser.add_argument("--candidate-compare-json", type=pathlib.Path, required=True)
    parser.add_argument("--workflow-compare-json", type=pathlib.Path, required=True)
    parser.add_argument("--release-json", type=pathlib.Path, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--product", required=True)
    parser.add_argument("--environment", required=True)
    parser.add_argument("--run-id", type=_positive, required=True)
    parser.add_argument("--run-attempt", type=_positive, required=True)
    parser.add_argument("--workflow-sha", required=True)
    parser.add_argument("--release-id", type=_positive, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--asset-manifest-sha256", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        envelope = validate(
            environment=load_json(args.environment_json, "environment response"),
            reviews=load_json(args.reviews_json, "review history"),
            workflow_run=load_json(args.workflow_run_json, "workflow-run response"),
            branch=load_json(args.branch_json, "default-branch response"),
            candidate_compare=load_json(
                args.candidate_compare_json, "candidate ancestry response"
            ),
            workflow_compare=load_json(
                args.workflow_compare_json, "workflow ancestry response"
            ),
            release=load_json(args.release_json, "release response"),
            repository=args.repository,
            product=args.product,
            environment_name=args.environment,
            run_id=args.run_id,
            run_attempt=args.run_attempt,
            workflow_sha=args.workflow_sha,
            release_id=args.release_id,
            tag=args.tag,
            commit=args.commit,
            asset_manifest_sha256=args.asset_manifest_sha256,
        )
        encoded = json.dumps(
            envelope, ensure_ascii=False, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        args.output.write_bytes(encoded)
        print(hashlib.sha256(encoded).hexdigest())
    except (ContractError, OSError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
