#!/usr/bin/env python3

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PRODUCT = ROOT.name
SCRIPT = ROOT / "scripts" / "check-windows-release-state.py"
SPEC = importlib.util.spec_from_file_location("windows_release_state", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("Cannot load Windows release-state gate")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)
POLICY = json.loads(
    (ROOT / "Installer" / "Windows" / "bootstrap-policy.json").read_text(encoding="utf-8")
)
BOOTSTRAP_TAG = POLICY["bootstrapTag"]
COMMIT_A = "1" * 40
COMMIT_B = "2" * 40


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def asset(name: str, tag: str, asset_id: int, **overrides: object) -> dict:
    value = {
        "browser_download_url": GATE.expected_asset_url(PRODUCT, tag, name),
        "digest": "sha256:" + "a" * 64,
        "id": asset_id,
        "name": name,
        "size": 4096,
        "state": "uploaded",
    }
    value.update(overrides)
    return value


def release(
    tag: str,
    release_id: int,
    *,
    windows: bool = False,
    draft: bool = False,
    prerelease: bool = False,
    immutable: bool = True,
    commit: str = COMMIT_A,
    assets: list[dict] | None = None,
    **overrides: object,
) -> dict:
    version = tag[1:]
    if assets is None:
        names = GATE.expected_release_asset_names(PRODUCT, version) if windows else ()
        assets = [asset(name, tag, release_id * 10 + index + 1)
                  for index, name in enumerate(names)]
    value = {
        "assets": assets,
        "draft": draft,
        "html_url": GATE.expected_release_url(PRODUCT, tag),
        "id": release_id,
        "immutable": immutable,
        "prerelease": prerelease,
        "tag_name": tag,
        "target_commitish": commit,
    }
    value.update(overrides)
    return value


def mac_release(tag: str, release_id: int, *, commit: str = "main") -> dict:
    name = f"{PRODUCT}-{tag[1:]}-macOS-universal.pkg"
    return release(tag, release_id, commit=commit,
                   assets=[asset(name, tag, release_id * 10 + 1)])


def resolve(
    releases: list[object],
    candidate_tag: str,
    *,
    confirmation: str = "false",
    allow_candidate_release_id: int | None = None,
    policy: object = POLICY,
    product: str = PRODUCT,
) -> dict:
    return GATE.resolve_release_state(
        releases,
        policy,
        product,
        candidate_tag,
        confirmation,
        allow_candidate_release_id,
    )


class WindowsReleaseStateTests(unittest.TestCase):
    def test_empty_history_bootstrap_and_canonical_cli_output(self) -> None:
        result = resolve([], BOOTSTRAP_TAG, confirmation="true")
        expected_history = {"product": PRODUCT, "releases": [], "schemaVersion": 1}
        self.assertEqual(
            result,
            {
                "baseline": None,
                "candidate": {"tag": BOOTSTRAP_TAG, "version": BOOTSTRAP_TAG[1:]},
                "historyDigest": "sha256:" + hashlib.sha256(
                    canonical_bytes(expected_history)
                ).hexdigest(),
                "mode": "bootstrap",
                "product": PRODUCT,
                "schemaVersion": 1,
            },
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            releases_path = root / "releases.json"
            policy_path = root / "policy.json"
            releases_path.write_text("[[]]", encoding="utf-8")
            policy_path.write_text(json.dumps(POLICY), encoding="utf-8")
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--releases-json", str(releases_path),
                    "--policy-json", str(policy_path),
                    "--product", PRODUCT,
                    "--candidate-tag", BOOTSTRAP_TAG,
                    "--confirmation", "true",
                ],
                check=False,
                capture_output=True,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr.decode(errors="replace"))
        self.assertEqual(completed.stdout, canonical_bytes(result) + b"\n")
        self.assertEqual(completed.stderr, b"")

    def test_bootstrap_requires_exact_policy_tag_product_and_confirmation(self) -> None:
        invalid = (
            (POLICY, PRODUCT, BOOTSTRAP_TAG, "false"),
            (POLICY, PRODUCT, "v9.9.9", "true"),
            ({**POLICY, "product": "WrongProduct"}, PRODUCT, BOOTSTRAP_TAG, "true"),
            ({**POLICY, "bootstrapTag": "v01.0.0"}, PRODUCT, BOOTSTRAP_TAG, "true"),
            ({**POLICY, "schemaVersion": 2}, PRODUCT, BOOTSTRAP_TAG, "true"),
            ({**POLICY, "schemaVersion": True}, PRODUCT, BOOTSTRAP_TAG, "true"),
            ({**POLICY, "extra": 1}, PRODUCT, BOOTSTRAP_TAG, "true"),
            (POLICY, "../unsafe", BOOTSTRAP_TAG, "true"),
            (POLICY, PRODUCT, BOOTSTRAP_TAG, "TRUE"),
        )
        for policy, product, tag, confirmation in invalid:
            with self.subTest(policy=policy, product=product, tag=tag,
                              confirmation=confirmation), self.assertRaises(GATE.ReleaseStateError):
                resolve([], tag, confirmation=confirmation, policy=policy, product=product)

    def test_upgrade_selects_highest_complete_stable_windows_release(self) -> None:
        older = release("v1.0.0", 10, windows=True, commit=COMMIT_A)
        latest = release("v1.2.3", 12, windows=True, commit=COMMIT_B)
        mac_only = mac_release("v1.3.0", 13)
        result = resolve([latest, mac_only, older], "v1.4.0")
        self.assertEqual(result["mode"], "upgrade")
        self.assertEqual(
            result["baseline"],
            {"commit": COMMIT_B, "releaseId": 12, "tag": "v1.2.3", "version": "1.2.3"},
        )
        self.assertEqual(result["candidate"], {"tag": "v1.4.0", "version": "1.4.0"})
        self.assertRegex(result["historyDigest"], r"^sha256:[0-9a-f]{64}$")

        # Bootstrap confirmation is irrelevant once a real Windows baseline exists.
        self.assertEqual(
            resolve([older], "v1.0.1", confirmation="true")["mode"], "upgrade"
        )

    def test_candidate_must_be_strictly_newer_and_within_msi_bounds(self) -> None:
        baseline = release("v1.2.3", 12, windows=True)
        with self.assertRaisesRegex(GATE.ReleaseStateError, "Candidate tag already"):
            resolve([baseline], "v1.2.3")
        with self.assertRaisesRegex(GATE.ReleaseStateError, "strictly newer"):
            resolve([baseline], "v1.2.2")
        for tag in ("1.2.4", "v01.2.4", "v1.2.4-rc1", "v256.0.0", "v1.256.0",
                    "v1.0.65536"):
            with self.subTest(tag=tag), self.assertRaises(GATE.ReleaseStateError):
                resolve([baseline], tag)

    def test_complete_windows_asset_set_is_exact(self) -> None:
        complete = release("v1.0.0", 10, windows=True)
        for index in range(len(complete["assets"])):
            partial = copy.deepcopy(complete)
            del partial["assets"][index]
            with self.subTest(missing=index), self.assertRaisesRegex(
                GATE.ReleaseStateError, "exact cross-platform asset set"
            ):
                resolve([partial], "v1.0.1")

        malformed_names = (
            f"{PRODUCT}-1.0.0-Windows-x86.msi",
            f"{PRODUCT}-1.0.0-Windows-x64.zip",
            f"{PRODUCT}-01.0.0-Windows-x64.msi",
            f"{PRODUCT}-1.0.1-Windows-x64.msi",
            f"{PRODUCT}-1.0.0-Windows-arm64ec.evidence.JSON",
            f"{PRODUCT}-1.0.0-windows-x64.msi",
            f"{PRODUCT.lower()}-1.0.0-Windows-x64.msi",
        )
        for index, name in enumerate(malformed_names):
            candidate = copy.deepcopy(complete)
            candidate["assets"][0] = asset(name, "v1.0.0", 900 + index)
            with self.subTest(name=name), self.assertRaises(GATE.ReleaseStateError):
                resolve([candidate], "v1.0.1")

        extra_name = f"{PRODUCT}-1.0.0-Windows-x64.symbols.zip"
        extra = copy.deepcopy(complete)
        extra["assets"].append(asset(extra_name, "v1.0.0", 999))
        with self.assertRaisesRegex(GATE.ReleaseStateError, "malformed Windows asset"):
            resolve([extra], "v1.0.1")

        extra = copy.deepcopy(complete)
        extra["assets"].append(asset(f"{PRODUCT}-1.0.0-notes.txt", "v1.0.0", 998))
        with self.assertRaisesRegex(GATE.ReleaseStateError, "exact cross-platform asset set"):
            resolve([extra], "v1.0.1")

    def test_every_asset_structure_is_validated_fail_closed(self) -> None:
        baseline = release("v1.0.0", 10, windows=True)
        mutations = {
            "non-object": None,
            "invalid id": {**baseline["assets"][0], "id": 0},
            "boolean id": {**baseline["assets"][0], "id": True},
            "unsafe name": {**baseline["assets"][0], "name": "../payload.msi"},
            "pending state": {**baseline["assets"][0], "state": "new"},
            "missing digest": {**baseline["assets"][0], "digest": None},
            "uppercase digest": {**baseline["assets"][0], "digest": "sha256:" + "A" * 64},
            "wrong algorithm": {**baseline["assets"][0], "digest": "sha512:" + "a" * 64},
            "zero size": {**baseline["assets"][0], "size": 0},
            "boolean size": {**baseline["assets"][0], "size": True},
            "foreign url": {**baseline["assets"][0], "browser_download_url":
                            "https://example.invalid/payload.msi"},
        }
        for name, mutation in mutations.items():
            candidate = copy.deepcopy(baseline)
            candidate["assets"][0] = mutation
            with self.subTest(name=name), self.assertRaises(GATE.ReleaseStateError):
                resolve([candidate], "v1.0.1")

        for index in (4, 7):
            for field, value in (
                ("state", "new"),
                ("digest", None),
                ("size", 0),
                ("browser_download_url", "https://example.invalid/payload"),
            ):
                candidate = copy.deepcopy(baseline)
                candidate["assets"][index][field] = value
                with self.subTest(asset=index, field=field), self.assertRaises(
                    GATE.ReleaseStateError
                ):
                    resolve([candidate], "v1.0.1")

        duplicate_name = copy.deepcopy(baseline)
        duplicate_name["assets"].append(copy.deepcopy(duplicate_name["assets"][0]))
        duplicate_name["assets"][-1]["id"] = 999
        with self.assertRaisesRegex(GATE.ReleaseStateError, "duplicate asset names"):
            resolve([duplicate_name], "v1.0.1")

        duplicate_id = [release("v1.0.0", 10, windows=True), mac_release("v1.0.1", 11)]
        duplicate_id[1]["assets"][0]["id"] = duplicate_id[0]["assets"][0]["id"]
        with self.assertRaisesRegex(GATE.ReleaseStateError, "duplicate asset IDs"):
            resolve(duplicate_id, "v1.0.2")

        legacy_mac = mac_release("legacy+mac/2020", 10)
        legacy_mac["assets"][0]["digest"] = None
        legacy_mac["assets"][0]["size"] = 0
        legacy_mac["assets"][0]["state"] = "legacy state"
        legacy_mac["assets"][0]["name"] = "old mac payload.pkg"
        legacy_mac["assets"][0]["browser_download_url"] = "https://legacy.invalid/payload"
        legacy_mac["html_url"] = "https://legacy.invalid/release"
        result = resolve([legacy_mac], BOOTSTRAP_TAG, confirmation="true")
        self.assertEqual(result["mode"], "bootstrap")

        changed_legacy = copy.deepcopy(legacy_mac)
        changed_legacy["html_url"] += "-changed"
        self.assertNotEqual(
            resolve([changed_legacy], BOOTSTRAP_TAG, confirmation="true")["historyDigest"],
            result["historyDigest"],
        )

        malformed_unrelated = copy.deepcopy(legacy_mac)
        malformed_unrelated["assets"][0]["digest"] = {"sha256": "a" * 64}
        with self.assertRaisesRegex(GATE.ReleaseStateError, "malformed digest"):
            resolve([malformed_unrelated], BOOTSTRAP_TAG, confirmation="true")
        missing_legacy_digest = copy.deepcopy(legacy_mac)
        del missing_legacy_digest["assets"][0]["digest"]
        with self.assertRaisesRegex(GATE.ReleaseStateError, "no digest field"):
            resolve([missing_legacy_digest], BOOTSTRAP_TAG, confirmation="true")

    def test_every_release_structure_and_identity_is_validated(self) -> None:
        baseline = release("v1.0.0", 10, windows=True)
        mutations: tuple[tuple[str, object], ...] = (
            ("non-object", None),
            ("zero id", {**baseline, "id": 0}),
            ("boolean id", {**baseline, "id": True}),
            ("flags", {**baseline, "draft": "false"}),
            ("immutable", {**baseline, "immutable": None}),
            ("assets", {**baseline, "assets": None}),
            ("tag", {**baseline, "tag_name": "v01.0.0"}),
            ("tag bound", {**baseline, "tag_name": "v256.0.0"}),
            ("target", {**baseline, "target_commitish": ""}),
            ("url", {**baseline, "html_url": "https://example.invalid/release"}),
        )
        for name, mutation in mutations:
            with self.subTest(name=name), self.assertRaises(GATE.ReleaseStateError):
                resolve([mutation], "v1.0.1")

        bad_commit = release("v1.0.0", 10, windows=True, commit="main")
        with self.assertRaisesRegex(GATE.ReleaseStateError, "exact commit"):
            resolve([bad_commit], "v1.0.1")
        self.assertEqual(
            resolve([mac_release("v1.0.0", 10, commit="release/main")],
                    BOOTSTRAP_TAG, confirmation="true")["mode"],
            "bootstrap",
        )

        duplicate_id = [release("v1.0.0", 10, windows=True), mac_release("v1.0.1", 10)]
        with self.assertRaisesRegex(GATE.ReleaseStateError, "duplicate IDs"):
            resolve(duplicate_id, "v1.0.2")
        duplicate_tag = [release("v1.0.0", 10, windows=True), mac_release("v1.0.0", 11)]
        with self.assertRaisesRegex(GATE.ReleaseStateError, "duplicate tags"):
            resolve(duplicate_tag, "v1.0.2")

    def test_draft_mutable_prerelease_and_candidate_collisions_are_rejected(self) -> None:
        draft = release("v1.0.0", 10, windows=True, draft=True, immutable=True)
        with self.assertRaisesRegex(GATE.ReleaseStateError, "Draft Windows"):
            resolve([draft], BOOTSTRAP_TAG, confirmation="true")

        mutable = release("v1.0.0", 10, windows=True, prerelease=True, immutable=False)
        with self.assertRaisesRegex(GATE.ReleaseStateError, "not immutable"):
            resolve([mutable], BOOTSTRAP_TAG, confirmation="true")

        mutable_stable = release("v1.0.0", 10, windows=True, immutable=False)
        with self.assertRaisesRegex(GATE.ReleaseStateError, "not immutable"):
            resolve([mutable_stable], "v1.0.1")

        # Even an asset-less release reserves the candidate tag and races creation.
        for flag in ("draft", "prerelease"):
            collision = release(BOOTSTRAP_TAG, 10, **{flag: True})
            with self.subTest(candidate=flag), self.assertRaisesRegex(
                GATE.ReleaseStateError, "Candidate tag already has a release"
            ):
                resolve([collision], BOOTSTRAP_TAG, confirmation="true")

        unrelated_prerelease = release("v1.0.0", 10, prerelease=True)
        result = resolve([unrelated_prerelease], BOOTSTRAP_TAG, confirmation="true")
        self.assertEqual(result["mode"], "bootstrap")

    def test_immutable_prereleases_are_quarantined_and_bound_candidate_version(self) -> None:
        quarantine = release(
            "v1.0.0", 10, windows=True, prerelease=True, immutable=True, commit=COMMIT_A
        )
        result = resolve([quarantine], "v1.0.1")
        self.assertEqual(result["mode"], "bootstrap")
        self.assertIsNone(result["baseline"])
        self.assertRegex(result["historyDigest"], r"^sha256:[0-9a-f]{64}$")
        promoted = release("v1.0.1", 11, windows=True, commit=COMMIT_B)
        self.assertEqual(
            resolve([quarantine, promoted], "v1.0.1", allow_candidate_release_id=11),
            result,
        )

        with self.assertRaisesRegex(GATE.ReleaseStateError, "strictly newer"):
            resolve([quarantine], "v0.9.9")
        with self.assertRaisesRegex(GATE.ReleaseStateError, "Candidate tag already"):
            resolve([quarantine], "v1.0.0")

        stable = release("v0.9.0", 9, windows=True, commit=COMMIT_B)
        upgraded = resolve([stable, quarantine], "v1.0.1")
        self.assertEqual(upgraded["mode"], "upgrade")
        self.assertEqual(upgraded["baseline"]["tag"], "v0.9.0")

        partial = copy.deepcopy(quarantine)
        partial["assets"].pop()
        with self.assertRaisesRegex(GATE.ReleaseStateError, "exact cross-platform"):
            resolve([partial], "v1.0.1")
        bad_commit = copy.deepcopy(quarantine)
        bad_commit["target_commitish"] = "main"
        with self.assertRaisesRegex(GATE.ReleaseStateError, "exact commit"):
            resolve([bad_commit], "v1.0.1")

    def test_post_promotion_recheck_allows_only_exact_candidate_and_is_stable(self) -> None:
        baseline = release("v1.0.0", 10, windows=True, commit=COMMIT_A)
        candidate = release("v1.1.0", 11, windows=True, commit=COMMIT_B)
        before = resolve([baseline], "v1.1.0")
        after = resolve([candidate, baseline], "v1.1.0", allow_candidate_release_id=11)
        self.assertEqual(after, before)

        with self.assertRaisesRegex(GATE.ReleaseStateError, "Candidate tag already"):
            resolve([candidate, baseline], "v1.1.0")
        for allowed_id in (0, -1, True, 999, 10):
            with self.subTest(allowed_id=allowed_id), self.assertRaises(GATE.ReleaseStateError):
                resolve([candidate, baseline], "v1.1.0",
                        allow_candidate_release_id=allowed_id)

        wrong_tag = release("v1.1.1", 11, windows=True)
        with self.assertRaisesRegex(GATE.ReleaseStateError, "does not match the candidate tag"):
            resolve([wrong_tag, baseline], "v1.1.0", allow_candidate_release_id=11)

        partial_candidate = copy.deepcopy(candidate)
        partial_candidate["assets"].pop()
        with self.assertRaisesRegex(GATE.ReleaseStateError, "exact cross-platform"):
            resolve([partial_candidate, baseline], "v1.1.0",
                    allow_candidate_release_id=11)

    def test_first_release_post_promotion_recheck_matches_bootstrap_state(self) -> None:
        candidate = release(BOOTSTRAP_TAG, 41, windows=True, commit=COMMIT_B)
        before = resolve([], BOOTSTRAP_TAG, confirmation="true")
        after = resolve([candidate], BOOTSTRAP_TAG, confirmation="true",
                        allow_candidate_release_id=41)
        self.assertEqual(after, before)
        with self.assertRaisesRegex(GATE.ReleaseStateError, "confirmation"):
            resolve([candidate], BOOTSTRAP_TAG, allow_candidate_release_id=41)

    def test_candidate_races_fail_or_change_the_canonical_state(self) -> None:
        baseline = release("v1.0.0", 10, windows=True)
        initial = resolve([baseline], "v1.3.0")
        intermediate = release("v1.1.0", 11, windows=True, commit=COMMIT_B)
        candidate = release("v1.3.0", 13, windows=True)
        raced = resolve([candidate, intermediate, baseline], "v1.3.0",
                        allow_candidate_release_id=13)
        self.assertNotEqual(raced, initial)
        self.assertEqual(raced["baseline"]["tag"], "v1.1.0")

        newer_race = release("v1.4.0", 14, windows=True)
        with self.assertRaisesRegex(GATE.ReleaseStateError, "strictly newer"):
            resolve([candidate, newer_race, baseline], "v1.3.0",
                    allow_candidate_release_id=13)

        duplicate_candidate = copy.deepcopy(candidate)
        duplicate_candidate["id"] = 99
        for index, entry in enumerate(duplicate_candidate["assets"]):
            entry["id"] = 990 + index
        with self.assertRaisesRegex(GATE.ReleaseStateError, "duplicate tags"):
            resolve([candidate, duplicate_candidate, baseline], "v1.3.0",
                    allow_candidate_release_id=13)

        stale = release("v1.2.0", 12, windows=True, prerelease=True, immutable=False)
        with self.assertRaisesRegex(GATE.ReleaseStateError, "not immutable"):
            resolve([candidate, stale, baseline], "v1.3.0",
                    allow_candidate_release_id=13)

        quarantine_race = release(
            "v1.2.0", 12, windows=True, prerelease=True, immutable=True
        )
        quarantined_state = resolve(
            [candidate, quarantine_race, baseline],
            "v1.3.0",
            allow_candidate_release_id=13,
        )
        self.assertNotEqual(quarantined_state["historyDigest"], initial["historyDigest"])

        quarantined_race = release(
            "v1.3.1", 131, windows=True, prerelease=True, immutable=True
        )
        with self.assertRaisesRegex(GATE.ReleaseStateError, "strictly newer"):
            resolve([candidate, quarantined_race, baseline], "v1.3.0",
                    allow_candidate_release_id=13)

    def test_history_digest_is_order_independent_and_binds_relevant_state(self) -> None:
        first = release("v1.0.0", 10, windows=True)
        second = mac_release("v1.1.0", 11)
        forward = resolve([first, second], "v1.2.0")
        reordered_first = copy.deepcopy(first)
        reordered_first["assets"].reverse()
        reverse = resolve([second, reordered_first], "v1.2.0")
        self.assertEqual(forward, reverse)

        changed = copy.deepcopy(first)
        changed["assets"][0]["digest"] = "sha256:" + "b" * 64
        self.assertNotEqual(
            resolve([changed, second], "v1.2.0")["historyDigest"],
            forward["historyDigest"],
        )
        added = release("v1.1.1", 12, prerelease=True)
        self.assertNotEqual(
            resolve([first, second, added], "v1.2.0")["historyDigest"],
            forward["historyDigest"],
        )

        # Mutable, non-contract API decoration cannot perturb the digest.
        decorated = copy.deepcopy(first)
        decorated["body"] = "mutable notes"
        decorated["assets"][0]["download_count"] = 999
        self.assertEqual(
            resolve([decorated, second], "v1.2.0")["historyDigest"],
            forward["historyDigest"],
        )

    def test_slurped_pagination_is_complete_bounded_and_unambiguous(self) -> None:
        first_page = [release(f"v0.0.{index}", index, commit="main")
                      for index in range(1, 101)]
        second_page = [release("v0.0.101", 101, commit="main")]
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "releases.json"
            path.write_text(json.dumps([first_page, second_page]), encoding="utf-8")
            loaded = GATE.load_paginated_releases(path)
            self.assertEqual(len(loaded), 101)
            self.assertEqual(
                resolve(loaded, BOOTSTRAP_TAG, confirmation="true")["mode"], "bootstrap"
            )

            malformed = (
                [],
                first_page,
                [None],
                [[first_page[0]], []],
                [[*first_page, second_page[0]]],
                [[] for _ in range(101)],
            )
            for value in malformed:
                with self.subTest(value_type=type(value), length=len(value)):
                    path.write_text(json.dumps(value), encoding="utf-8")
                    with self.assertRaises(GATE.ReleaseStateError):
                        GATE.load_paginated_releases(path)

            duplicate_page = copy.deepcopy(first_page)
            duplicate_page.append(copy.deepcopy(first_page[0]))
            path.write_text(json.dumps([first_page, [duplicate_page[-1]]]), encoding="utf-8")
            with self.assertRaisesRegex(GATE.ReleaseStateError, "duplicate IDs"):
                resolve(GATE.load_paginated_releases(path), BOOTSTRAP_TAG, confirmation="true")

            path.write_text("", encoding="utf-8")
            with self.assertRaisesRegex(GATE.ReleaseStateError, "invalid size"):
                GATE.load_paginated_releases(path)

            path.write_text('[[{"id":1,"id":2}]]', encoding="utf-8")
            with self.assertRaisesRegex(GATE.ReleaseStateError, "duplicate key"):
                GATE.load_paginated_releases(path)

            path.write_text("[[NaN]]", encoding="utf-8")
            with self.assertRaisesRegex(GATE.ReleaseStateError, "non-finite"):
                GATE.load_paginated_releases(path)

    def test_linked_metadata_and_cli_failures_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            target = root / "target.json"
            target.write_text("[[]]", encoding="utf-8")
            linked = root / "linked.json"
            try:
                linked.symlink_to(target)
            except OSError as error:
                self.skipTest(f"Symlinks are unavailable on this runner: {error}")
            with self.assertRaisesRegex(GATE.ReleaseStateError, "missing or linked"):
                GATE.load_paginated_releases(linked)

            policy_path = root / "policy.json"
            policy_path.write_text(json.dumps(POLICY), encoding="utf-8")
            malformed_path = root / "malformed.json"
            malformed_path.write_text("{}", encoding="utf-8")
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--releases-json", str(malformed_path),
                    "--policy-json", str(policy_path),
                    "--product", PRODUCT,
                    "--candidate-tag", BOOTSTRAP_TAG,
                    "--confirmation", "true",
                ],
                check=False,
                capture_output=True,
            )
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(completed.stdout, b"")
        self.assertIn(b"Windows release-state gate failed:", completed.stderr)


if __name__ == "__main__":
    unittest.main()
