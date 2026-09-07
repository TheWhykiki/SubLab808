#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PRODUCT = ROOT.name
SCRIPT = ROOT / "scripts" / "check-windows-release-bootstrap.py"
SPEC = importlib.util.spec_from_file_location("windows_release_bootstrap", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("Cannot load Windows bootstrap gate")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)
POLICY = json.loads(
    (ROOT / "Installer" / "Windows" / "bootstrap-policy.json").read_text(encoding="utf-8")
)


def release(
    *names: str,
    draft: bool = False,
    prerelease: bool = False,
    release_id: int | None = None,
    tag_name: str | None = None,
) -> dict:
    value = {
        "draft": draft,
        "prerelease": prerelease,
        "assets": [{"name": name} for name in names],
    }
    if release_id is not None:
        value["id"] = release_id
    if tag_name is not None:
        value["tag_name"] = tag_name
    return value


class WindowsReleaseBootstrapTests(unittest.TestCase):
    def test_policy_permanently_limits_bootstrap_to_declared_product_and_tag(self) -> None:
        GATE.verify_bootstrap_policy(POLICY, PRODUCT, POLICY["bootstrapTag"])
        mutations = (
            (POLICY, PRODUCT, "v9.9.9"),
            (POLICY, "WrongProduct", POLICY["bootstrapTag"]),
            ({**POLICY, "unexpected": True}, PRODUCT, POLICY["bootstrapTag"]),
            ({**POLICY, "schemaVersion": 2}, PRODUCT, POLICY["bootstrapTag"]),
        )
        for policy, product, tag in mutations:
            with self.subTest(policy=policy, product=product, tag=tag), self.assertRaises(
                GATE.BootstrapError
            ):
                GATE.verify_bootstrap_policy(policy, product, tag)

    def test_empty_or_macos_only_history_is_a_valid_bootstrap(self) -> None:
        GATE.verify_no_windows_baseline([], PRODUCT)
        GATE.verify_no_windows_baseline(
            [release(f"{PRODUCT}-1.0.4-macOS-universal.pkg")], PRODUCT
        )

    def test_draft_and_prerelease_windows_assets_are_not_updater_baselines(self) -> None:
        name = f"{PRODUCT}-1.2.3-Windows-x64.msi"
        GATE.verify_no_windows_baseline(
            [release(name, draft=True), release(name, prerelease=True)], PRODUCT
        )

    def test_any_stable_canonical_windows_asset_blocks_bootstrap(self) -> None:
        for suffix in (
            "Windows-x64.msi",
            "Windows-arm64ec.msi",
            "Windows-x64.evidence.json",
            "Windows-arm64ec.evidence.json",
        ):
            with self.subTest(suffix=suffix), self.assertRaisesRegex(
                GATE.BootstrapError, "exact N-to-N\\+1 updater acceptance is required"
            ):
                GATE.verify_no_windows_baseline(
                    [release(f"{PRODUCT}-1.2.3-{suffix}")], PRODUCT
                )

    def test_post_publish_audit_allows_only_the_exact_created_release(self) -> None:
        tag = POLICY["bootstrapTag"]
        own = release(
            f"{PRODUCT}-{tag[1:]}-Windows-x64.msi",
            release_id=41,
            tag_name=tag,
        )
        GATE.verify_no_windows_baseline([own], PRODUCT, 41, tag)

        competitor = release(f"{PRODUCT}-9.9.9-Windows-arm64ec.msi")
        with self.assertRaisesRegex(GATE.BootstrapError, "N-to-N\\+1"):
            GATE.verify_no_windows_baseline([own, competitor], PRODUCT, 41, tag)

        invalid_histories = (
            [],
            [release(release_id=41, tag_name="v9.9.9")],
            [release(release_id=41, tag_name=tag, draft=True)],
            [own, own],
        )
        for history in invalid_histories:
            with self.subTest(history=history), self.assertRaises(GATE.BootstrapError):
                GATE.verify_no_windows_baseline(history, PRODUCT, 41, tag)

        for release_id, allowed_tag in ((0, tag), (-1, tag), (41, "not-a-tag"), (41, None)):
            with self.subTest(release_id=release_id, tag=allowed_tag), self.assertRaises(
                GATE.BootstrapError
            ):
                GATE.verify_no_windows_baseline([own], PRODUCT, release_id, allowed_tag)

    def test_noncanonical_or_nonrelease_names_do_not_create_a_false_baseline(self) -> None:
        GATE.verify_no_windows_baseline(
            [
                release(f"{PRODUCT}-01.2.3-Windows-x64.msi"),
                release(f"{PRODUCT}-1.2.3-Windows-x86.msi"),
                release(f"{PRODUCT}-1.2.3-Windows-x64-UNSIGNED.msi"),
            ],
            PRODUCT,
        )

    def test_full_page_and_malformed_metadata_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "releases.json"
            path.write_text(json.dumps([release()] * 100), encoding="utf-8")
            with self.assertRaisesRegex(GATE.BootstrapError, "complete public release history"):
                GATE.load_releases(path)
        malformed = (
            [None],
            [{"draft": False, "prerelease": False, "assets": None}],
            [{"draft": "false", "prerelease": False, "assets": []}],
            [{"draft": False, "prerelease": False, "assets": [{}]}],
        )
        for value in malformed:
            with self.subTest(value=value), self.assertRaises(GATE.BootstrapError):
                GATE.verify_no_windows_baseline(value, PRODUCT)

    def test_slurped_pagination_is_complete_bounded_and_unambiguous(self) -> None:
        first_page = [release(release_id=index) for index in range(1, 101)]
        second_page = [release(release_id=101)]
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "paginated.json"
            path.write_text(json.dumps([first_page, second_page]), encoding="utf-8")
            self.assertEqual(len(GATE.load_releases(path, paginated=True)), 101)

            malformed_pages = (
                [],
                [release(release_id=1)],
                [[release()]],
                [[release(release_id=1), release(release_id=1)]],
                [[release(release_id=index) for index in range(1, 100)], second_page],
            )
            for value in malformed_pages:
                with self.subTest(value=value):
                    path.write_text(json.dumps(value), encoding="utf-8")
                    with self.assertRaises(GATE.BootstrapError):
                        GATE.load_releases(path, paginated=True)

    def test_linked_metadata_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            target = root / "target.json"
            target.write_text("[]", encoding="utf-8")
            linked = root / "linked.json"
            try:
                linked.symlink_to(target)
            except OSError as error:
                self.skipTest(f"Symlinks are unavailable on this runner: {error}")
            with self.assertRaisesRegex(GATE.BootstrapError, "missing or linked"):
                GATE.load_releases(linked)


if __name__ == "__main__":
    unittest.main()
