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
spec = importlib.util.spec_from_file_location("release_protection", SCRIPT)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class ReleaseProtectionTests(unittest.TestCase):
    def setUp(self):
        self.branch = {
            "enforce_admins": {"enabled": True},
            "required_pull_request_reviews": {"required_approving_review_count": 1, "dismiss_stale_reviews": True},
            "required_status_checks": {"strict": True, "contexts": ["Shared release contract parity"],
                                       "checks": [{"context": "macOS universal build + arm64 tests", "app_id": 15368}]},
            "allow_force_pushes": {"enabled": False}, "allow_deletions": {"enabled": False}}
        self.signing = self.environment("release-signing", 41)
        self.physical = self.environment("physical-daw-release", 42)

    @staticmethod
    def environment(name, identifier):
        return {"name": name, "id": identifier, "can_admins_bypass": False,
                "deployment_branch_policy": {"protected_branches": True, "custom_branch_policies": False},
                "protection_rules": [{"type": "required_reviewers", "prevent_self_review": True,
                                      "reviewers": [{"type": "User", "reviewer": {
                                          "id": 900, "login": "independent-reviewer", "type": "User"}}]}]}

    def validate(self):
        return gate.validate(self.branch, self.signing, self.physical, "TheWhykiki", "release-operator")

    def test_valid_current_api_shapes(self):
        result = self.validate()
        self.assertEqual(result["release-signing"], ["independent-reviewer"])
        self.assertEqual(len(result["requiredChecks"]), 2)

    def test_missing_or_weakened_branch_protection_rejected(self):
        variants = [None, {}, {**self.branch, "enforce_admins": {"enabled": False}},
                    {**self.branch, "required_pull_request_reviews": None},
                    {**self.branch, "required_status_checks": None},
                    {**self.branch, "allow_force_pushes": {"enabled": True}},
                    {**self.branch, "allow_deletions": {"enabled": True}}]
        for value in variants:
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.validate_branch(value)

    def test_current_independent_review_mandatory(self):
        for changes in ({"required_approving_review_count": 0}, {"required_approving_review_count": True},
                        {"required_approving_review_count": 7}, {"dismiss_stale_reviews": False},
                        {"bypass_pull_request_allowances": {"users": [{"login": "TheWhykiki"}]}},
                        {"bypass_pull_request_allowances": {"apps": [{"id": 1}]}}):
            branch = copy.deepcopy(self.branch)
            branch["required_pull_request_reviews"].update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
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

    def test_self_review_and_duplicate_rules_rejected(self):
        for mutation in ("self", "duplicate", "absent", "empty", "team"):
            environment = copy.deepcopy(self.signing)
            rule = environment["protection_rules"][0]
            if mutation == "self":
                rule["prevent_self_review"] = False
            elif mutation == "duplicate":
                environment["protection_rules"].append(copy.deepcopy(rule))
            elif mutation == "absent":
                del rule["prevent_self_review"]
            elif mutation == "empty":
                rule["reviewers"] = []
            else:
                rule["reviewers"][0]["type"] = "Team"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                gate.validate_environment(environment, "release-signing", {"thewhykiki"})

    def test_owner_or_actor_cannot_be_optional_alternative_reviewer(self):
        for excluded in ("TheWhykiki", "THEWHYKIKI", "release-operator"):
            environment = copy.deepcopy(self.signing)
            environment["protection_rules"][0]["reviewers"].append(
                {"type": "User", "reviewer": {"id": 901, "login": excluded}})
            with self.subTest(excluded=excluded), self.assertRaises(ValueError):
                gate.validate(self.branch, environment, self.physical, "TheWhykiki", "release-operator")

    def test_duplicate_reviewer_ids_or_logins_rejected(self):
        for reviewer in ({"id": 900, "login": "another-reviewer"},
                         {"id": 901, "login": "INDEPENDENT-REVIEWER"}):
            environment = copy.deepcopy(self.signing)
            environment["protection_rules"][0]["reviewers"].append({"type": "User", "reviewer": reviewer})
            with self.subTest(reviewer=reviewer), self.assertRaises(ValueError):
                gate.validate_environment(environment, "release-signing", {"thewhykiki"})

    def test_cli_reads_exact_files_and_rejects_ambiguous_json(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            command = [sys.executable, "-B", str(SCRIPT), "--owner", "TheWhykiki", "--actor", "release-operator"]
            for name, value in (("branch-protection", self.branch), ("signing-environment", self.signing),
                                ("physical-environment", self.physical)):
                path = root / (name + ".json")
                path.write_text(json.dumps(value))
                command += ["--" + name + "-json", str(path)]
            success = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(success.returncode, 0, success.stderr)
            path = root / "signing-environment.json"
            path.write_text(json.dumps(self.signing)[:-1] + ',"can_admins_bypass":true}')
            denied = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(denied.returncode, 0)
            self.assertIn("Duplicate", denied.stderr)


if __name__ == "__main__":
    unittest.main()
