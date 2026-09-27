# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Whykiki Audio
"""Protection-removal regressions against the documented GitHub REST shapes."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "validate-release-protection.py"
OWNER = "TheWhykiki"
OWNER_ID = 12602174
spec = importlib.util.spec_from_file_location("release_protection", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class ReleaseProtectionTests(unittest.TestCase):
    def setUp(self):
        self.branch = {
            "enforce_admins": {"enabled": True},
            "required_pull_request_reviews": {"required_approving_review_count": 0, "dismiss_stale_reviews": True,
                                             "require_last_push_approval": False, "require_code_owner_reviews": False},
            "required_status_checks": {"strict": True, "contexts": ["Shared release contract parity"],
                                       "checks": [{"context": "macOS universal build + arm64 tests", "app_id": 15368}]},
            "allow_force_pushes": {"enabled": False}, "allow_deletions": {"enabled": False}}
        self.signing = self.environment("release-signing", 41)
        self.physical = self.environment("physical-daw-release", 42)

    @staticmethod
    def environment(name, identifier):
        return {"name": name, "id": identifier, "can_admins_bypass": False,
                "deployment_branch_policy": {"protected_branches": True, "custom_branch_policies": False},
                "protection_rules": [{"type": "required_reviewers", "prevent_self_review": False,
                                      "reviewers": [{"type": "User", "reviewer": {
                                          "id": OWNER_ID, "login": OWNER, "type": "User"}}]}]}

    def validate(self):
        return gate.validate(self.branch, self.signing, self.physical, OWNER, OWNER, OWNER_ID)

    def test_valid_current_api_shapes(self):
        result = self.validate()
        self.assertEqual(result["release-signing"], ["thewhykiki"])
        self.assertEqual(result["physical-daw-release"], ["thewhykiki"])
        self.assertEqual(len(result["requiredChecks"]), 2)

    def test_owner_may_trigger_and_approve_but_nonowner_actor_cannot_replace_owner(self):
        for actor in (OWNER, "THEWHYKIKI", "release-operator"):
            with self.subTest(actor=actor):
                result = gate.validate(self.branch, self.signing, self.physical, OWNER, actor, OWNER_ID)
                self.assertEqual(result["release-signing"], ["thewhykiki"])

    def test_owner_login_is_case_insensitive_but_id_is_not_optional(self):
        self.signing["protection_rules"][0]["reviewers"][0]["reviewer"]["login"] = "THEWHYKIKI"
        self.assertEqual(self.validate()["release-signing"], ["thewhykiki"])
        for invalid in (None, True, False, 0, -1, "12602174", 12602174.0, OWNER_ID + 1):
            with self.subTest(owner_id=invalid), self.assertRaises(ValueError):
                gate.validate(self.branch, self.signing, self.physical, OWNER, OWNER, invalid)
        for invalid in (None, False, "", " owner", "owner/other", "different-owner"):
            with self.subTest(owner=invalid), self.assertRaises(ValueError):
                gate.validate(self.branch, self.signing, self.physical, invalid, OWNER, OWNER_ID)

    def test_missing_or_weakened_branch_protection_rejected(self):
        variants = [None, {}, {**self.branch, "enforce_admins": {"enabled": False}},
                    {**self.branch, "required_pull_request_reviews": None},
                    {**self.branch, "required_status_checks": None},
                    {**self.branch, "allow_force_pushes": {"enabled": True}},
                    {**self.branch, "allow_deletions": {"enabled": True}}]
        for value in variants:
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.validate_branch(value)

    def test_exact_owner_only_branch_policy_is_mandatory(self):
        for changes in ({"required_approving_review_count": 1}, {"required_approving_review_count": True},
                        {"required_approving_review_count": False}, {"required_approving_review_count": "0"},
                        {"required_approving_review_count": 0.0}, {"required_approving_review_count": None},
                        {"required_approving_review_count": -1}, {"required_approving_review_count": 7},
                        {"dismiss_stale_reviews": False}, {"dismiss_stale_reviews": 1},
                        {"require_last_push_approval": True}, {"require_last_push_approval": 0},
                        {"require_last_push_approval": None}, {"require_code_owner_reviews": True},
                        {"require_code_owner_reviews": 0}, {"require_code_owner_reviews": None},
                        {"bypass_pull_request_allowances": {"users": [{"login": "TheWhykiki"}]}},
                        {"bypass_pull_request_allowances": {"teams": [{"id": 1}]}},
                        {"bypass_pull_request_allowances": {"apps": [{"id": 1}]}}):
            branch = copy.deepcopy(self.branch)
            branch["required_pull_request_reviews"].update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                gate.validate_branch(branch)
        for key in ("required_approving_review_count", "dismiss_stale_reviews",
                    "require_last_push_approval", "require_code_owner_reviews"):
            branch = copy.deepcopy(self.branch)
            del branch["required_pull_request_reviews"][key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                gate.validate_branch(branch)

    def test_status_checks_nonempty_and_strict(self):
        variants = ({"strict": False, "contexts": ["test"]}, {"strict": True},
                    {"strict": True, "contexts": [""]}, {"strict": True, "contexts": [False]},
                    {"strict": True, "checks": [{"context": "test", "app_id": True}]},
                    {"strict": True, "checks": [{}]}, {"strict": True, "checks": "test"})
        for checks in variants:
            with self.subTest(checks=checks), self.assertRaises(ValueError):
                gate.validate_branch(dict(self.branch, required_status_checks=checks))

    def test_checks_api_without_legacy_contexts_is_supported(self):
        checks = {"strict": True, "checks": [{"context": "native tests", "app_id": None}]}
        self.assertEqual(gate.validate_branch(dict(self.branch, required_status_checks=checks)), ["native tests"])

    def test_both_environments_require_protection(self):
        for attribute in ("signing", "physical"):
            original = copy.deepcopy(getattr(self, attribute))
            for changes in ({"name": "unprotected"}, {"id": True}, {"can_admins_bypass": True},
                            {"protection_rules": []}, {"deployment_branch_policy": None},
                            {"deployment_branch_policy": {"protected_branches": False, "custom_branch_policies": True}}):
                setattr(self, attribute, dict(original, **changes))
                with self.subTest(environment=attribute, changes=changes), self.assertRaises(ValueError):
                    self.validate()
            setattr(self, attribute, original)

    def test_exact_self_review_boolean_and_single_reviewer_rule_required(self):
        for mutation in ("self", "duplicate", "absent", "empty", "team", "zero", "null", "string"):
            environment = copy.deepcopy(self.signing)
            rule = environment["protection_rules"][0]
            if mutation == "self":
                rule["prevent_self_review"] = True
            elif mutation == "duplicate":
                environment["protection_rules"].append(copy.deepcopy(rule))
            elif mutation == "absent":
                del rule["prevent_self_review"]
            elif mutation == "empty":
                rule["reviewers"] = []
            elif mutation == "team":
                rule["reviewers"][0]["type"] = "Team"
            else:
                rule["prevent_self_review"] = {"zero": 0, "null": None, "string": "false"}[mutation]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                gate.validate_environment(environment, "release-signing", OWNER, OWNER_ID)

    def test_foreign_missing_or_malformed_owner_identity_rejected_in_both_environments(self):
        for attribute in ("signing", "physical"):
            original = copy.deepcopy(getattr(self, attribute))
            for changes in ({"id": OWNER_ID + 1}, {"id": True}, {"id": 0}, {"id": -1},
                            {"id": str(OWNER_ID)}, {"id": float(OWNER_ID)}, {"id": None},
                            {"login": "release-operator"}, {"login": "independent-reviewer"},
                            {"login": "TheWhykiki/other"}, {"login": " TheWhykiki"}, {"login": None},
                            {"type": "Organization"}, {"type": "Bot"}, {"type": None}):
                environment = copy.deepcopy(original)
                environment["protection_rules"][0]["reviewers"][0]["reviewer"].update(changes)
                setattr(self, attribute, environment)
                with self.subTest(environment=attribute, changes=changes), self.assertRaises(ValueError):
                    self.validate()
            for key in ("id", "login", "type"):
                environment = copy.deepcopy(original)
                del environment["protection_rules"][0]["reviewers"][0]["reviewer"][key]
                setattr(self, attribute, environment)
                with self.subTest(environment=attribute, missing=key), self.assertRaises(ValueError):
                    self.validate()
            setattr(self, attribute, original)

    def test_duplicate_or_alternative_owner_reviewers_rejected(self):
        for reviewer in ({"id": OWNER_ID, "login": OWNER, "type": "User"},
                         {"id": OWNER_ID, "login": "another-reviewer", "type": "User"},
                         {"id": OWNER_ID + 1, "login": OWNER, "type": "User"},
                         {"id": 901, "login": "independent-reviewer", "type": "User"}):
            environment = copy.deepcopy(self.signing)
            environment["protection_rules"][0]["reviewers"].append({"type": "User", "reviewer": reviewer})
            with self.subTest(reviewer=reviewer), self.assertRaises(ValueError):
                gate.validate_environment(environment, "release-signing", OWNER, OWNER_ID)

    def test_reviewer_containers_and_types_must_be_exact(self):
        for reviewers in (None, {}, "TheWhykiki", [None], [{}], [{"type": "User", "reviewer": None}],
                          [{"reviewer": {"id": OWNER_ID, "login": OWNER, "type": "User"}}]):
            environment = copy.deepcopy(self.signing)
            environment["protection_rules"][0]["reviewers"] = reviewers
            with self.subTest(reviewers=reviewers), self.assertRaises(ValueError):
                gate.validate_environment(environment, "release-signing", OWNER, OWNER_ID)

    def test_all_workflow_preflights_bind_trusted_owner_id(self):
        workflow = (SCRIPT.parents[1] / ".github/workflows/windows-release.yml").read_text()
        self.assertEqual(workflow.count("WK_REPOSITORY_OWNER_ID: ${{ github.repository_owner_id }}"), 3)
        calls = workflow.split("python3 -B scripts/validate-release-protection.py")[1:]
        self.assertEqual(len(calls), 3)
        for call in calls:
            invocation = call.split("\n\n", 1)[0]
            self.assertEqual(invocation.count('--owner-id "$WK_REPOSITORY_OWNER_ID"'), 1)

    def test_cli_reads_exact_files_and_rejects_ambiguous_json(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            command = [sys.executable, "-B", str(SCRIPT), "--owner", OWNER, "--actor", OWNER,
                       "--owner-id", str(OWNER_ID)]
            for name, value in (("branch-protection", self.branch), ("signing-environment", self.signing),
                                ("physical-environment", self.physical)):
                path = root / (name + ".json")
                path.write_text(json.dumps(value))
                command += ["--" + name + "-json", str(path)]
            success = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(success.returncode, 0, success.stderr)
            self.assertIn("owner-only", success.stdout)
            owner_id_index = command.index("--owner-id")
            for invalid in ("0", "-1", "true", "12602174.0", str(OWNER_ID + 1)):
                altered = command.copy()
                altered[owner_id_index + 1] = invalid
                with self.subTest(owner_id=invalid):
                    denied = subprocess.run(altered, capture_output=True, text=True)
                    self.assertNotEqual(denied.returncode, 0)
            missing = command[:owner_id_index] + command[owner_id_index + 2:]
            denied = subprocess.run(missing, capture_output=True, text=True)
            self.assertNotEqual(denied.returncode, 0)
            path = root / "signing-environment.json"
            path.write_text(json.dumps(self.signing)[:-1] + ',"can_admins_bypass":true}')
            denied = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(denied.returncode, 0)
            self.assertIn("Duplicate", denied.stderr)


if __name__ == "__main__":
    unittest.main()
