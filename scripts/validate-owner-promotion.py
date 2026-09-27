#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Require a separate, exact owner approval before Stable/Latest promotion."""
from __future__ import annotations
import argparse
import json
import pathlib
import re

PREFIX = "whykiki-stable-promotion-v2:"

def parse_json(raw):
    def unique_keys(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError(f"Duplicate JSON key: {key}")
            value[key] = item
        return value
    def reject_constant(value):
        raise ValueError(f"Non-JSON numeric constant: {value}")
    return json.loads(raw, object_pairs_hook=unique_keys, parse_constant=reject_constant)

def validate(pages, repository_metadata, expected):
    if (not isinstance(repository_metadata, dict)
            or repository_metadata.get("full_name") != expected["repository"]):
        raise ValueError("Repository identity mismatch")
    owner = repository_metadata.get("owner", {})
    if (not isinstance(owner, dict) or owner.get("type") != "User" or type(owner.get("id")) is not int
            or owner["id"] <= 0 or owner.get("login") != expected["repository"].split("/")[0]):
        raise ValueError("An individual repository owner is required")
    if not isinstance(pages, list) or not pages or len(pages) > 100:
        raise ValueError("Complete paginated comments are required")
    ids, decisions = set(), []
    for i, page in enumerate(pages):
        if not isinstance(page, list) or len(page) > 100 or (i < len(pages)-1 and len(page) != 100):
            raise ValueError("Malformed or incomplete comment pagination")
        for comment in page:
            if not isinstance(comment, dict) or type(comment.get("id")) is not int or comment["id"] <= 0:
                raise ValueError("Invalid comment identity")
            if comment["id"] in ids:
                raise ValueError("Duplicate comment identity")
            ids.add(comment["id"])
            user = comment.get("user", {})
            if not isinstance(user, dict):
                raise ValueError("Malformed comment author")
            if user.get("id") != owner["id"] or user.get("login") != owner["login"]:
                continue
            body = comment.get("body", "")
            if not isinstance(body, str) or not body.startswith(PREFIX):
                continue
            if len(body) > 4096 or comment.get("commit_id") != expected["commit"]:
                raise ValueError("Malformed owner approval")
            try:
                approval = parse_json(body[len(PREFIX):])
            except (ValueError, TypeError) as exc:
                raise ValueError("Malformed owner approval JSON") from exc
            if not isinstance(approval, dict):
                raise ValueError("Owner approval must be an object")
            binding = {k: v for k, v in approval.items() if k != "decision"}
            # Ignore approval for another run, never reuse it.
            if binding != expected:
                continue
            if approval.get("decision") not in ("approve", "reject"):
                raise ValueError("Invalid promotion decision")
            if any(type(approval.get(k)) is not int for k in ("schemaVersion", "runId", "runAttempt", "releaseId")):
                raise ValueError("Promotion IDs must be integers")
            decisions.append((comment["id"], approval["decision"]))
    if not decisions or max(decisions)[1] != "approve":
        raise ValueError("No current explicit owner approval for this immutable candidate")
    return max(decisions)[0]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--comments", type=pathlib.Path, required=True)
    p.add_argument("--repository-json", type=pathlib.Path, required=True)
    p.add_argument("--repository", required=True)
    for name in ("run-id", "run-attempt", "release-id"):
        p.add_argument("--"+name, type=int, required=True)
    for name in ("tag", "commit", "asset-manifest-sha256"):
        p.add_argument("--"+name, required=True)
    a = p.parse_args()
    if (not re.fullmatch(r"[A-Za-z0-9-]+/(SubLab808|ReverseLab)", a.repository)
            or not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", a.tag)
            or not re.fullmatch(r"[0-9a-f]{40}", a.commit)
            or not re.fullmatch(r"[0-9a-f]{64}", a.asset_manifest_sha256)
            or min(a.run_id, a.run_attempt, a.release_id) <= 0):
        p.error("Invalid expected candidate binding")
    expected = dict(schemaVersion=2, repository=a.repository, runId=a.run_id,
                    runAttempt=a.run_attempt, releaseId=a.release_id, tag=a.tag,
                    commit=a.commit, assetManifestSha256=a.asset_manifest_sha256)
    try:
        if a.comments.stat().st_size > 16*1024*1024 or a.repository_json.stat().st_size > 1024*1024:
            raise ValueError("Oversized approval metadata")
        comment_id = validate(parse_json(a.comments.read_text()), parse_json(a.repository_json.read_text()), expected)
    except (OSError, ValueError) as exc:
        p.exit(1, f"Owner promotion denied: {exc}\n")
    print(f"Explicit owner approval verified: comment {comment_id}")

if __name__ == "__main__":
    main()
