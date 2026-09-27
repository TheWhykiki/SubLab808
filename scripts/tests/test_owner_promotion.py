# SPDX-License-Identifier: AGPL-3.0-only
import copy
import importlib.util
import json
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("owner_gate", pathlib.Path(__file__).resolve().parents[1] / "validate-owner-promotion.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)

class OwnerPromotionTests(unittest.TestCase):
    def setUp(self):
        self.binding = dict(schemaVersion=2, repository="TheWhykiki/SubLab808", runId=123,
                            runAttempt=1, releaseId=456, tag="v1.4.0", commit="a"*40,
                            assetManifestSha256="b"*64)
        self.repo = dict(full_name=self.binding["repository"], owner=dict(type="User", id=55, login="TheWhykiki"))
        self.comment = dict(id=17, user=self.repo["owner"].copy(), commit_id="a"*40,
                            body=gate.PREFIX + json.dumps(dict(self.binding, decision="approve")))
    def test_exact_owner_approval(self):
        self.assertEqual(gate.validate([[self.comment]], self.repo, self.binding), 17)
    def test_missing_or_unrelated_approval_denied(self):
        with self.assertRaises(ValueError):
            gate.validate([[]], self.repo, self.binding)
        for key, value in (("runAttempt",2),("releaseId",457),("commit","c"*40),("assetManifestSha256","c"*64)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                gate.validate([[self.comment]], self.repo, dict(self.binding, **{key:value}))
    def test_non_owner_or_spoofed_login_denied(self):
        for changes in (dict(id=99),dict(login="outsider")):
            comment = copy.deepcopy(self.comment)
            comment["user"].update(changes)
            with self.assertRaises(ValueError):
                gate.validate([[comment]], self.repo, self.binding)
    def test_later_rejection_revokes_approval(self):
        rejection = dict(self.comment, id=18, body=gate.PREFIX+json.dumps(dict(self.binding, decision="reject")))
        with self.assertRaises(ValueError):
            gate.validate([[rejection,self.comment]], self.repo, self.binding)
    def test_malformed_duplicate_and_wrong_commit(self):
        variants = ([[self.comment,self.comment]], [[dict(self.comment, commit_id="d"*40)]],
                    [[dict(self.comment,body=gate.PREFIX+"{")]], [[],[self.comment]])
        for pages in variants:
            with self.subTest(pages=pages), self.assertRaises(ValueError):
                gate.validate(pages, self.repo, self.binding)
    def test_boolean_ids_denied(self):
        binding = dict(self.binding,runAttempt=True)
        comment = dict(self.comment, body=gate.PREFIX+json.dumps(dict(binding,decision="approve")))
        with self.assertRaises(ValueError):
            gate.validate([[comment]],self.repo,self.binding)

    def test_ambiguous_duplicate_owner_decision_denied(self):
        approval = json.dumps(dict(self.binding, decision="reject"))
        body = gate.PREFIX + approval[:-1] + ',"decision":"approve"}'
        with self.assertRaises(ValueError):
            gate.validate([[dict(self.comment, body=body)]], self.repo, self.binding)

    def test_duplicate_metadata_and_non_json_numbers_denied(self):
        for raw in ('{"id":55,"id":99}', '{"body":{"decision":"reject","decision":"approve"}}',
                    '{"id":NaN}', '{"id":Infinity}'):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                gate.parse_json(raw)

if __name__ == "__main__":
    unittest.main()
