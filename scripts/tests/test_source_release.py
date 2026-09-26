"""Actual Git/ZIP provenance and adversarial archive tests; no network/signing needed."""
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 Whykiki Audio
import copy
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_contract as contract
import source_release as source


class SourceReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo, self.juce = self.base / "product", self.base / "juce"
        for repo in (self.repo, self.juce):
            repo.mkdir()
            self.git(repo, "init", "--quiet")
        self.write(self.juce, "CMakeLists.txt", "project(JUCE)\n")
        self.write(self.juce, "LICENSE.md", "Upstream license remains intact\n")
        self.write(self.juce, "modules/test.cpp", "// upstream code\n")
        self.commit(self.juce)
        self.juce_commit = self.git(self.juce, "rev-parse", "HEAD").strip()
        self.addCleanup(patch.stopall)
        patch.object(source, "JUCE_COMMIT", self.juce_commit).start()
        patch.object(contract, "JUCE_COMMIT", self.juce_commit).start()
        config = contract.strict_json((Path(__file__).resolve().parents[2] / "release/product.json").read_bytes())
        config["juce"]["commit"] = self.juce_commit
        self.config = config
        self.write(self.repo, "release/product.json", source.canonical_json(config))
        for name, data in {
                "CMakeLists.txt": f"project({config['productName']} VERSION {config['version']})\n",
                "LICENSE": "AGPL-3.0-only\n", "THIRD_PARTY_NOTICES.md": "Notices\n",
                "Presets/FactoryPresets.json": "{}\n", "Source/test.cpp": "// original\n",
                "scripts/source-release.py": "#!/usr/bin/env python3\n",
                "scripts/source_release.py": "# source implementation\n",
                "scripts/release_contract.py": "# contract implementation\n",
                "Design/QA/reference.png": b"non-build reference image",
                ".gitignore": "build/\n.env\n"}.items():
            self.write(self.repo, name, data)
        self.commit(self.repo)
        self.commit_id = self.git(self.repo, "rev-parse", "HEAD").strip()
        self.archive = self.base / "first.zip"

    @staticmethod
    def git(repo, *arguments):
        return subprocess.check_output(["git", "-C", str(repo), *arguments], stderr=subprocess.PIPE).decode()

    def commit(self, repo):
        self.git(repo, "add", ".")
        self.git(repo, "-c", "user.name=Source Test", "-c", "user.email=source@example.invalid",
                 "commit", "--quiet", "-m", "Fixture")

    @staticmethod
    def write(root, name, data):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)

    def create(self):
        return source.create_archive(self.repo, self.juce, self.archive, self.commit_id)

    def rewrite(self, mutate):
        with zipfile.ZipFile(self.archive) as archive:
            entries = [(copy.copy(info), archive.read(info)) for info in archive.infolist()]
        destination = self.base / "mutated.zip"
        with zipfile.ZipFile(destination, "w") as archive:
            for info, data in mutate(entries):
                archive.writestr(info, data)
        return destination

    def test_reproducible_across_mtime_and_output_name(self):
        manifest = self.create()
        (self.repo / "Source/test.cpp").touch()
        second = self.base / "second.zip"
        source.create_archive(self.repo, self.juce, second)
        self.assertEqual(self.archive.read_bytes(), second.read_bytes())
        self.assertEqual(manifest["commit"], self.commit_id)
        self.assertEqual(manifest["juceCommit"], self.juce_commit)
        self.assertEqual(manifest["excludedNonBuildFiles"], ["Design/QA/reference.png"])
        source.verify_archive(second, self.commit_id)

    def test_untracked_ignored_credentials_and_builds_never_enter_archive(self):
        self.write(self.repo, ".env", "secret-value")
        self.write(self.repo, "build/binary", b"binary")
        self.create()
        with zipfile.ZipFile(self.archive) as archive:
            names = archive.namelist()
        self.assertFalse(any("/.git/" in name or "/.env" in name or "/build/" in name for name in names))

    def test_dirty_product_rejected(self):
        self.write(self.repo, "Source/test.cpp", "changed")
        with self.assertRaisesRegex(source.SourceError, "clean"):
            self.create()
        self.assertFalse(self.archive.exists())

    def test_dirty_vendor_rejected(self):
        self.write(self.juce, "modules/test.cpp", "changed")
        with self.assertRaisesRegex(source.SourceError, "clean"):
            self.create()

    def test_wrong_juce_commit_rejected(self):
        patch.object(source, "JUCE_COMMIT", "a" * 40).start()
        with self.assertRaisesRegex(source.SourceError, "required source commit"):
            self.create()

    def test_wrong_expected_product_commit_rejected(self):
        with self.assertRaisesRegex(source.SourceError, "required source commit"):
            source.create_archive(self.repo, self.juce, self.archive, "b" * 40)

    def test_existing_output_is_never_overwritten(self):
        self.archive.write_bytes(b"keep")
        with self.assertRaisesRegex(source.SourceError, "replace"):
            self.create()
        self.assertEqual(self.archive.read_bytes(), b"keep")

    def test_tracked_private_key_rejected(self):
        self.write(self.repo, "Source/credential.txt", b"-----BEGIN " + b"PRIVATE KEY-----\ncredential\n")
        self.commit(self.repo)
        with self.assertRaisesRegex(source.SourceError, "private key"):
            source.create_archive(self.repo, self.juce, self.archive)

    def test_tracked_secret_extension_rejected(self):
        self.write(self.repo, "scripts/secret.p12", b"secret")
        self.commit(self.repo)
        with self.assertRaisesRegex(source.SourceError, "Credential"):
            source.create_archive(self.repo, self.juce, self.archive)

    def test_tracked_symlink_rejected(self):
        try:
            (self.repo / "Source/link.cpp").symlink_to("test.cpp")
        except (OSError, NotImplementedError):
            self.skipTest("Host cannot create test symlinks")
        self.commit(self.repo)
        with self.assertRaisesRegex(source.SourceError, "symlink"):
            source.create_archive(self.repo, self.juce, self.archive)

    def test_safe_cross_platform_paths(self):
        for name in ("../escape", "/absolute", "a\\b", "a/./b", "a//b", "a/.git/config",
                     "a/.GIT/config",
                     "a/CON.txt", "a/Lpt9", "a/name.", "a/name ", "C:/file", "a/line\nbreak"):
            with self.subTest(name=name), self.assertRaises(source.SourceError):
                source.safe_path(name)

    def test_corrupted_source_rejected(self):
        self.create()
        path = self.rewrite(lambda entries: [(info, b"tampered" if info.filename.endswith("/Source/test.cpp") else data)
                                            for info, data in entries])
        with self.assertRaisesRegex(source.SourceError, "differ"):
            source.verify_archive(path)

    def test_duplicate_zip_member_rejected(self):
        self.create()
        with self.assertWarns(UserWarning):
            path = self.rewrite(lambda entries: entries + [entries[0]])
        with self.assertRaisesRegex(source.SourceError, "Duplicate"):
            source.verify_archive(path)

    def test_zip_traversal_rejected(self):
        self.create()
        def mutation(entries):
            entries[0][0].filename = "../escape"
            return entries
        with self.assertRaises(source.SourceError):
            source.verify_archive(self.rewrite(mutation))

    def test_zip_symlink_rejected(self):
        self.create()
        def mutation(entries):
            entries[0][0].external_attr = (stat.S_IFLNK | 0o777) << 16
            return entries
        with self.assertRaisesRegex(source.SourceError, "metadata"):
            source.verify_archive(self.rewrite(mutation))

    def test_deflated_zip_is_not_accepted_as_canonical_source(self):
        self.create()
        def mutation(entries):
            entries[0][0].compress_type = zipfile.ZIP_DEFLATED
            return entries
        with self.assertRaisesRegex(source.SourceError, "metadata"):
            source.verify_archive(self.rewrite(mutation))

    def test_missing_inventory_rejected(self):
        self.create()
        path = self.rewrite(lambda entries: [(info, data) for info, data in entries
                                            if not info.filename.endswith("/" + source.MANIFEST)])
        with self.assertRaisesRegex(source.SourceError, "no provenance"):
            source.verify_archive(path)

    def test_archive_expected_commit_checked(self):
        self.create()
        with self.assertRaisesRegex(source.SourceError, "different commit"):
            source.verify_archive(self.archive, "a" * 40)

    def test_extracted_tree_and_exact_juce_tree(self):
        self.create()
        extracted = self.base / "extracted"
        with zipfile.ZipFile(self.archive) as archive:
            archive.extractall(extracted)
        root = next(extracted.iterdir())
        source.verify_tree(root)
        source.verify_tree(root, juce_only=True)
        self.write(root, "build/cache", "cache is allowed outside vendor source")
        source.verify_tree(root)
        self.write(root, "external/JUCE/injected.cmake", "execute_process(evil)")
        with self.assertRaisesRegex(source.SourceError, "unmanifested"):
            source.verify_tree(root, juce_only=True)

    def test_extracted_source_modification_rejected(self):
        self.create()
        extracted = self.base / "extracted"
        with zipfile.ZipFile(self.archive) as archive:
            archive.extractall(extracted)
        root = next(extracted.iterdir())
        self.write(root, "external/JUCE/modules/test.cpp", "// changed code!\n")
        with self.assertRaises(source.SourceError):
            source.verify_tree(root, juce_only=True)

    def test_extracted_unlisted_own_source_rejected(self):
        self.create()
        extracted = self.base / "extracted"
        with zipfile.ZipFile(self.archive) as archive:
            archive.extractall(extracted)
        root = next(extracted.iterdir())
        self.write(root, "Source/unlisted.cpp", "// injected")
        with self.assertRaisesRegex(source.SourceError, "unmanifested"):
            source.verify_tree(root)

    def test_unicode_and_case_alias_keys_match_filesystem_semantics(self):
        self.assertEqual(source.path_key("Source/Caf\u00e9.cpp"), source.path_key("source/Cafe\u0301.cpp"))

    def test_spdx_inventory_lists_vendor_without_relicensing(self):
        self.create()
        with zipfile.ZipFile(self.archive) as archive:
            document = json.loads(next(archive.read(name) for name in archive.namelist()
                                       if name.endswith("/" + source.SBOM)))
        self.assertEqual(document["spdxVersion"], "SPDX-2.3")
        vendor = next(item for item in document["packages"] if item["name"] == "JUCE")
        self.assertEqual(vendor["licenseDeclared"], "NOASSERTION")
        for item in document["files"]:
            self.assertEqual(len(item["checksums"]), 2)
            if item["fileName"].startswith("./external/JUCE/"):
                self.assertEqual(item["licenseConcluded"], "NOASSERTION")


class ContractTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[2]
        self.config = contract.load_product(self.root)

    def test_duplicate_json_keys_rejected(self):
        with self.assertRaises(contract.ContractError):
            contract.strict_json('{"productName":"a","productName":"b"}')

    def test_config_rejects_bad_version_identity_license_and_pin(self):
        for key, value in (("version", "1.0.0-rc"), ("repository", "attacker/product"),
                           ("license", "proprietary"), ("release-contract-version", True),
                           ("pluginCode", "bad"), ("juce", {})):
            config = copy.deepcopy(self.config)
            config[key] = value
            with self.subTest(key=key), self.assertRaises(contract.ContractError):
                contract.validate_product(config)

    def test_legacy_identifiers_match_configuration(self):
        contract.verify_product_projection(self.root)

    def test_parity_rejects_changed_shared_file(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for name in ("first", "second"):
                root = base / name
                for path in ("release/product.json", "Installer/Windows/package-config.json", "CMakeLists.txt"):
                    target = root / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes((self.root / path).read_bytes())
                (root / "release/shared-files.json").write_bytes(source.canonical_json(
                    {"release-contract-version": 2, "files": ["shared.py"], "productNormalizedFiles": []}))
                (root / "shared.py").write_text("same")
            contract.verify_parity(base / "first", base / "second")
            (base / "second/shared.py").write_text("drift")
            with self.assertRaisesRegex(contract.ContractError, "drift"):
                contract.verify_parity(base / "first", base / "second")

    def test_workflow_guard_mutation_rejected_by_product_normalized_parity(self):
        workflow = ".github/workflows/windows-release.yml"
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for name in ("first", "second"):
                root = base / name
                for path in ("release/product.json", "Installer/Windows/package-config.json", "CMakeLists.txt", workflow):
                    target = root / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes((self.root / path).read_bytes())
                (root / "release/shared-files.json").write_bytes(source.canonical_json(
                    {"release-contract-version": 2, "files": [], "productNormalizedFiles": [workflow]}))
            contract.verify_parity(base / "first", base / "second")
            target = base / "second" / workflow
            original = target.read_text()
            self.assertIn("inputs.confirm_release == true", original)
            target.write_text(original.replace("inputs.confirm_release == true", "inputs.confirm_release == false", 1))
            with self.assertRaisesRegex(contract.ContractError, "windows-release.yml"):
                contract.verify_parity(base / "first", base / "second")

    def test_only_the_four_declared_workflow_identity_values_are_normalized(self):
        peer = copy.deepcopy(self.config)
        peer["productName"] = "PeerPlugin"
        peer["upgradeCodes"] = {"x64": "00000000-0000-5000-8000-000000000001",
                                "arm64ec": "00000000-0000-5000-8000-000000000002"}
        def workflow(config):
            return (f"{config['productName']}\n{config['productName'].upper()}\n"
                    f"{config['upgradeCodes']['x64']}\n{config['upgradeCodes']['arm64ec']}\n"
                    "if: explicitApproval == true\n").encode()
        path = ".github/workflows/windows-release.yml"
        self.assertEqual(contract.normalize_product_file(workflow(self.config), path, self.config),
                         contract.normalize_product_file(workflow(peer), path, peer))
        self.assertNotEqual(contract.normalize_product_file(workflow(self.config), path, self.config),
                            contract.normalize_product_file(workflow(peer).replace(b"true", b"false"), path, peer))
        transition = "scripts/test-windows-updater-transition.ps1"
        self.assertEqual(contract.normalize_product_file(self.config["productName"].encode(), transition, self.config),
                         contract.normalize_product_file(peer["productName"].encode(), transition, peer))
        self.assertNotEqual(contract.normalize_product_file(self.config["productName"].upper().encode(), transition, self.config),
                            contract.normalize_product_file(peer["productName"].upper().encode(), transition, peer))
        with self.assertRaises(contract.ContractError):
            contract.normalize_product_file(b"@@WK_PRODUCT_NAME@@", path, self.config)


if __name__ == "__main__":
    unittest.main()
