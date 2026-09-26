#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Whykiki Audio
"""Fail closed before signing/public staging if GitHub protection is weakened.

Consumes unmodified GET /branches/{branch}/protection and
GET /environments/{name} REST responses (Administration:read token).
"""
import argparse
import json
from pathlib import Path
import re

LOGIN = re.compile(r"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$")


def object_value(value, label):
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def positive(value, label):
    if type(value) is not int or value <= 0:
        raise ValueError(f"{label} must be a positive integer")
    return value


def login(value, label):
    if not isinstance(value, str) or not LOGIN.fullmatch(value):
        raise ValueError(f"{label} must be a canonical individual GitHub login")
    return value.casefold()


def load_json(path):
    if path.is_symlink() or path.stat().st_size > 2 * 1024 * 1024:
        raise ValueError("Protection response is linked or oversized")
    def unique_keys(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError(f"Duplicate JSON key: {key}")
            value[key] = item
        return value
    def reject_constant(value):
        raise ValueError(f"Invalid JSON number: {value}")
    return json.loads(path.read_bytes(), object_pairs_hook=unique_keys, parse_constant=reject_constant)


def validate_branch(value):
    value = object_value(value, "Branch protection")
    if object_value(value.get("enforce_admins"), "Admin enforcement").get("enabled") is not True:
        raise ValueError("Branch protection must enforce restrictions for administrators")
    reviews = object_value(value.get("required_pull_request_reviews"), "Required PR reviews")
    count = positive(reviews.get("required_approving_review_count"), "Required review count")
    if count > 6 or reviews.get("dismiss_stale_reviews") is not True:
        raise ValueError("At least one current PR review with stale-review dismissal is required")
    bypass = reviews.get("bypass_pull_request_allowances")
    if bypass is not None:
        bypass = object_value(bypass, "PR bypass allowances")
        for kind in ("users", "teams", "apps"):
            if kind in bypass and bypass[kind] != []:
                raise ValueError("PR review bypass allowances must be empty")
    checks = object_value(value.get("required_status_checks"), "Required status checks")
    if checks.get("strict") is not True:
        raise ValueError("Required checks must require an up-to-date branch")
    contexts = checks.get("contexts", [])
    check_objects = checks.get("checks", [])
    if not isinstance(contexts, list) or not isinstance(check_objects, list):
        raise ValueError("Required checks are malformed")
    names = list(contexts)
    for check in check_objects:
        check = object_value(check, "Required check")
        app_id = check.get("app_id")
        if app_id is not None and (type(app_id) is not int or app_id == 0 or app_id < -1):
            raise ValueError("Required check app ID is malformed")
        names.append(check.get("context"))
    if not names or len(names) > 200 or any(not isinstance(name, str) or not name.strip()
                                          or len(name) > 255 or any(ord(c) < 32 for c in name)
                                          for name in names):
        raise ValueError("At least one named required status check is required")
    for key in ("allow_force_pushes", "allow_deletions"):
        if key in value and object_value(value[key], key).get("enabled") is not False:
            raise ValueError("Protected release branch must prohibit force pushes and deletion")
    return sorted(set(names))


def validate_environment(value, name, forbidden):
    value = object_value(value, name)
    positive(value.get("id"), name + " id")
    if value.get("name") != name or value.get("can_admins_bypass") is not False:
        raise ValueError(f"{name} must have the expected identity and forbid administrator bypass")
    branches = object_value(value.get("deployment_branch_policy"), name + " branch policy")
    if branches.get("protected_branches") is not True or branches.get("custom_branch_policies") is not False:
        raise ValueError(f"{name} must allow only protected branches")
    rules = value.get("protection_rules")
    if not isinstance(rules, list):
        raise ValueError(f"{name} protection rules are missing")
    reviewer_rules = []
    for rule in rules:
        rule = object_value(rule, name + " protection rule")
        if rule.get("type") == "required_reviewers":
            reviewer_rules.append(rule)
    if len(reviewer_rules) != 1 or reviewer_rules[0].get("prevent_self_review") is not True:
        raise ValueError(f"{name} must require reviewers and prevent self-review")
    reviewers = reviewer_rules[0].get("reviewers")
    if not isinstance(reviewers, list) or not 1 <= len(reviewers) <= 6:
        raise ValueError(f"{name} must name 1-6 independent individual reviewers")
    ids, logins = set(), set()
    for entry in reviewers:
        entry = object_value(entry, name + " reviewer entry")
        reviewer = object_value(entry.get("reviewer"), name + " reviewer")
        reviewer_id = positive(reviewer.get("id"), name + " reviewer id")
        reviewer_login = login(reviewer.get("login"), name + " reviewer login")
        if entry.get("type") != "User" or reviewer.get("type", "User") != "User" \
                or reviewer_login in forbidden or reviewer_id in ids or reviewer_login in logins:
            raise ValueError(f"{name} reviewers must be distinct individuals independent of owner and actor")
        ids.add(reviewer_id)
        logins.add(reviewer_login)
    return sorted(logins)


def validate(branch_protection, signing_environment, physical_environment, owner, actor):
    forbidden = {login(owner, "Repository owner"), login(actor, "Workflow actor")}
    return {"requiredChecks": validate_branch(branch_protection),
            "release-signing": validate_environment(signing_environment, "release-signing", forbidden),
            "physical-daw-release": validate_environment(physical_environment, "physical-daw-release", forbidden)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("branch-protection", "signing-environment", "physical-environment"):
        parser.add_argument("--" + name + "-json", type=Path, required=True)
    parser.add_argument("--owner", required=True)
    parser.add_argument("--actor", required=True)
    args = parser.parse_args()
    try:
        validate(load_json(args.branch_protection_json), load_json(args.signing_environment_json),
                 load_json(args.physical_environment_json), args.owner, args.actor)
    except (OSError, ValueError, UnicodeError) as error:
        parser.exit(1, f"Release protection rejected: {error}\n")
    print("Protected branch and independent signing/physical approval policies verified")


if __name__ == "__main__":
    main()
