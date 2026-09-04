"""Pipeline contracts with fake macOS tools; no credentials or DAW required."""
import importlib.util
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("release_pipeline", Path(__file__).resolve().parents[1] / "release_pipeline.py")
pipeline = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pipeline)


class FakeTools:
    def __init__(self, fail=None, version="1.0.4", arches="arm64", notary_status="Accepted",
                 corrupt=None, fail_host_at=None):
        self.commands = []
        self.fail, self.version, self.arches = fail, version, arches
        self.notary_status = notary_status
        self.corrupt, self.fail_host_at, self.host_calls = corrupt, fail_host_at, 0
        self.source = self.payload = self.archived = None

    def __call__(self, command, *, cwd=None, capture=False):
        parts = [str(value) for value in command]
        self.commands.append(parts)
        tool = Path(parts[0]).name
        if tool == "SubLab808HostTests":
            self.host_calls += 1
            if self.host_calls == self.fail_host_at:
                raise subprocess.CalledProcessError(1, parts)
        if self.fail == tool:
            raise subprocess.CalledProcessError(1, parts)
        output = ""
        if any(Path(part).name == "generate-presets.py" for part in parts[1:]):
            # Exercise the actual generator against the actual staged inputs.
            return subprocess.run(parts, cwd=cwd, check=True, text=True, capture_output=True)
        if tool == "cmake" and "-S" in parts:
            self.source = Path(parts[parts.index("-S") + 1])
        elif tool == "cmake" and "--build" in parts:
            build = Path(parts[2])
            bundle = build / "SubLab808_artefacts/Release/VST3/SubLab808.vst3"
            binary = bundle / "Contents/MacOS/SubLab808"
            binary.parent.mkdir(parents=True)
            binary.write_bytes((self.source / "Source/processor.cpp").read_bytes())
            binary.chmod(0o755)
            (bundle / "Contents/Info.plist").write_bytes(plistlib.dumps({"CFBundleShortVersionString": self.version}))
            helper = bundle / "Contents/Helpers/SubLab808Updater.app"
            helper_binary = helper / "Contents/MacOS/SubLab808Updater"
            helper_binary.parent.mkdir(parents=True)
            helper_binary.write_bytes(b"test updater")
            helper_binary.chmod(0o755)
            (helper / "Contents/Info.plist").write_bytes(plistlib.dumps({
                "CFBundleShortVersionString": self.version, "WKProduct": "SubLab808"}))
        elif tool == "ctest":
            Path(parts[parts.index("--output-junit") + 1]).write_text('<testsuite tests="1" failures="0"/>\n')
            test_log = Path(parts[parts.index("--test-dir") + 1]) / "Testing/Temporary/LastTest.log"
            test_log.parent.mkdir(parents=True)
            test_log.write_text("Fake test log for pipeline unit tests\n")
        elif tool == "lipo":
            output = self.arches
        elif tool == "xcrun" and parts[1] == "vtool":
            output = "minos 11.0\n"
        elif tool == "xcrun" and parts[1] == "notarytool":
            output = json.dumps({"status": self.notary_status})
        elif tool == "ditto":
            if "-c" in parts:
                self.archived = Path(parts[-2])
                Path(parts[-1]).write_bytes(b"fake ZIP")
            elif "-x" in parts:
                shutil.copytree(self.archived, Path(parts[-1]) / "SubLab808.vst3")
                if self.corrupt == "zip":
                    (Path(parts[-1]) / "SubLab808.vst3/Contents/MacOS/SubLab808").write_bytes(b"corrupt")
            else:
                shutil.copytree(parts[1], parts[2])
        elif tool == "pkgbuild":
            self.payload = Path(parts[parts.index("--root") + 1])
            components = plistlib.loads(Path(parts[parts.index("--component-plist") + 1]).read_bytes())
            if components != [{"RootRelativeBundlePath": "Library/Audio/Plug-Ins/VST3/SubLab808.vst3",
                               "BundleIsRelocatable": False, "BundleIsVersionChecked": True,
                               "BundleHasStrictIdentifier": True, "BundleOverwriteAction": "upgrade"}]:
                raise AssertionError("Installer must never relocate the plugin")
            Path(parts[-1]).write_bytes(b"fake PKG")
        elif tool == "pkgutil":
            shutil.copytree(self.payload, Path(parts[-1]) / "Payload")
            if self.corrupt == "payload":
                (Path(parts[-1]) / "Payload/Library/Audio/Plug-Ins/VST3/SubLab808.vst3/Contents/MacOS/SubLab808").write_bytes(b"corrupt")
        return subprocess.CompletedProcess(parts, 0, stdout=output)


class ReleasePipelineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "Source").mkdir()
        (self.root / "CMakeLists.txt").write_text("project(SubLab808 VERSION 1.0.4 LANGUAGES C CXX)\n")
        (self.root / "Source/processor.cpp").write_text("current source\n")
        repository = Path(__file__).resolve().parents[2]
        for relative in ("scripts/generate-presets.py", "Presets/FactoryPresets.json", "Source/FactoryBank.h"):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(repository / relative, target)
        (self.root / ".gitignore").write_text("/dist/\n/.release-candidate-*/\n")
        for command in (["git", "init", "-q"], ["git", "add", "."],
                        ["git", "-c", "user.name=Pipeline Test", "-c", "user.email=pipeline@example.invalid",
                         "commit", "-qm", "fixture"]):
            subprocess.run(command, cwd=self.root, check=True, capture_output=True)
        (self.root / "dist").mkdir()
        self.previous = self.root / "dist/previous.pkg"
        self.previous.write_bytes(b"keep previous release")
        self.addCleanup(patch.stopall)
        patch.object(pipeline.platform, "system", return_value="Darwin").start()
        patch.object(pipeline.shutil, "which", return_value="/fake/tool").start()

    def package(self, tools=None, environment=None, **options):
        return pipeline.package_release(self.root, runner=tools or FakeTools(),
                                        environment=environment or {}, **options)

    def assertPreviousPreserved(self):
        self.assertEqual(self.previous.read_bytes(), b"keep previous release")
        for entry in (self.root / "dist").iterdir():
            if entry == self.previous:
                continue
            self.assertTrue(entry.is_dir() and entry.name.startswith("failed-run-"), entry)
            self.assertEqual(json.loads((entry / "failure.json").read_text())["status"], "failed")
            self.assertFalse((entry / "release-manifest.json").exists())
        self.assertEqual(list(self.root.glob(".release-candidate-*")), [])
        self.assertEqual(list((self.root / "dist").glob(".release-candidate-*")), [])

    def test_failed_ctest_preserves_source_and_test_reports(self):
        tools = FakeTools()

        def fail_after_report(command, **options):
            result = tools(command, **options)
            parts = [str(value) for value in command]
            if parts[0] == "ctest":
                Path(parts[parts.index("--output-junit") + 1]).write_text('<testsuite tests="1" failures="1"/>\n')
                raise subprocess.CalledProcessError(8, parts)
            return result

        with self.assertRaises(subprocess.CalledProcessError) as raised:
            self.package(fail_after_report)
        self.assertEqual(raised.exception.returncode, 8)
        reports = list((self.root / "dist").glob("failed-run-*"))
        self.assertEqual(len(reports), 1, "Failed-run evidence must survive temporary build cleanup")
        report = reports[0]
        details = json.loads((report / "failure.json").read_text())
        source = json.loads((report / "source-manifest.json").read_text())
        self.assertEqual((details["status"], details["step"], details["tool"], details["exit_code"]),
                         ("failed", "ctest", "ctest", 8))
        self.assertEqual(details["commit"], source["repositories"][0]["commit"])
        self.assertEqual(details["source_sha256"], source["source_sha256"])
        self.assertIn('failures="1"', (report / "ctest-results.xml").read_text())
        self.assertIn("Fake test log", (report / "CTest-LastTest.log").read_text())
        for name, expected in details["artifacts"].items():
            self.assertEqual(pipeline.file_hash(report / name), expected)
        self.assertFalse(any(Path(command[0]).name == "pkgbuild" for command in tools.commands))
        self.assertPreviousPreserved()

    def test_host_failures_keep_distinct_diagnostics_not_release_candidates(self):
        for invocation, step in ((1, "zip_host"), (2, "installer_host")):
            with self.assertRaises(subprocess.CalledProcessError):
                self.package(FakeTools(fail_host_at=invocation))
            reports = list((self.root / "dist").glob("failed-run-*"))
            self.assertEqual(len(reports), invocation)
            matching = [folder for folder in reports
                        if json.loads((folder / "failure.json").read_text())["step"] == step]
            self.assertEqual(len(matching), 1)
            self.assertTrue((matching[0] / "ctest-results.xml").is_file())
            self.assertTrue((matching[0] / "CTest-LastTest.log").is_file())
            self.assertFalse(any(matching[0].glob("*.pkg")))
            self.assertFalse(any(matching[0].glob("*.zip")))
            self.assertPreviousPreserved()

    def test_snapshot_failure_has_no_fabricated_source_identity(self):
        with patch.object(pipeline, "snapshot_source", side_effect=pipeline.ReleaseError("changed source")):
            with self.assertRaisesRegex(pipeline.ReleaseError, "changed source"):
                self.package()
        report, = (self.root / "dist").glob("failed-run-*")
        details = json.loads((report / "failure.json").read_text())
        self.assertEqual(details["step"], "source_snapshot")
        self.assertIsNone(details["commit"])
        self.assertIsNone(details["source_sha256"])
        self.assertFalse((report / "source-manifest.json").exists())
        self.assertPreviousPreserved()

    def test_diagnostics_failure_never_masks_original_tool_failure(self):
        with patch.object(pipeline, "save_failure_evidence", side_effect=OSError("disk full")):
            with self.assertRaises(subprocess.CalledProcessError) as raised:
                self.package(FakeTools(fail="ctest"))
        self.assertEqual(Path(raised.exception.cmd[0]).name, "ctest")
        self.assertEqual(raised.exception.returncode, 1)
        self.assertPreviousPreserved()

    def test_failure_report_does_not_record_signing_or_notary_arguments(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.package(FakeTools(fail="pkgbuild"),
                         {"SUBLAB808_APPLICATION_IDENTITY": "private-app-identity-marker",
                          "SUBLAB808_INSTALLER_IDENTITY": "private-installer-identity-marker",
                          "SUBLAB808_NOTARY_PROFILE": "private-notary-profile-marker"})
        report, = (self.root / "dist").glob("failed-run-*")
        text = (report / "failure.json").read_text()
        for marker in ("private-app-identity-marker", "private-installer-identity-marker", "private-notary-profile-marker"):
            self.assertNotIn(marker, text)
        self.assertPreviousPreserved()

    def test_invalid_configuration_fails_before_tools_or_dist_changes(self):
        for options, environment in [({"configuration": "Debug"}, {}),
                                     ({"version_override": "2.0.0"}, {}),
                                     ({}, {"SUBLAB808_NOTARY_PROFILE": "profile"}),
                                     ({}, {"SUBLAB808_APPLICATION_IDENTITY": "app"}),
                                     ({}, {"SUBLAB808_APPLICATION_IDENTITY": "-", "SUBLAB808_INSTALLER_IDENTITY": "-"}),
                                     ({}, {"SUBLAB808_BUILD_JOBS": "0"})]:
            with self.subTest(options=options, environment=environment):
                tools = FakeTools()
                with self.assertRaises(pipeline.ReleaseError):
                    self.package(tools, environment, **options)
                self.assertEqual(tools.commands, [])
                self.assertPreviousPreserved()

    def test_build_test_sign_package_and_host_failures_preserve_previous_artifacts(self):
        for tool in ("cmake", "ctest", "codesign", "pkgbuild", "pkgutil", "SubLab808HostTests"):
            with self.subTest(tool=tool):
                with self.assertRaises(subprocess.CalledProcessError):
                    self.package(FakeTools(fail=tool))
                self.assertPreviousPreserved()

    def test_stale_factory_recipe_fails_before_build_and_preserves_previous_artifacts(self):
        recipes = self.root / "Presets/FactoryPresets.json"
        data = json.loads(recipes.read_text())
        data["presets"][0]["name"] = "Changed recipe without regenerated header"
        recipes.write_text(json.dumps(data))
        for optimization in ("", "1", "2"):
            with self.subTest(optimization=optimization), patch.dict(pipeline.os.environ, {"PYTHONOPTIMIZE": optimization}):
                tools = FakeTools()
                with self.assertRaises(subprocess.CalledProcessError) as raised:
                    self.package(tools)
                self.assertIn("FactoryBank.h is stale", raised.exception.stderr)
                self.assertFalse(any(Path(command[0]).name == "cmake" for command in tools.commands))
                self.assertPreviousPreserved()

    def test_wrong_bundle_version_or_architecture_never_publishes(self):
        for tools in (FakeTools(version="1.0.3"), FakeTools(arches="x86_64")):
            with self.assertRaises(pipeline.ReleaseError):
                self.package(tools)
            self.assertPreviousPreserved()

    def test_missing_embedded_updater_never_publishes(self):
        tools = FakeTools()
        def remove_helper(command, **options):
            result = tools(command, **options)
            parts = [str(value) for value in command]
            if parts[0] == "cmake" and "--build" in parts:
                shutil.rmtree(Path(parts[2]) / "SubLab808_artefacts/Release/VST3/SubLab808.vst3/Contents/Helpers")
            return result
        with self.assertRaises(OSError):
            self.package(remove_helper)
        self.assertPreviousPreserved()

    def test_notary_invalid_status_never_publishes_even_if_command_exits_zero(self):
        with self.assertRaisesRegex(pipeline.ReleaseError, "not accepted"):
            self.package(FakeTools(notary_status="Invalid"),
                         {"SUBLAB808_APPLICATION_IDENTITY": "app", "SUBLAB808_INSTALLER_IDENTITY": "installer",
                          "SUBLAB808_NOTARY_PROFILE": "profile"})
        self.assertPreviousPreserved()

    def test_corrupted_zip_or_installer_payload_is_not_published(self):
        for location in ("zip", "payload"):
            with self.subTest(location=location):
                with self.assertRaisesRegex(pipeline.ReleaseError, "differs"):
                    self.package(FakeTools(corrupt=location))
                self.assertPreviousPreserved()

    def test_zip_and_installer_payload_host_failures_preserve_old_release(self):
        for invocation in (1, 2):
            with self.subTest(invocation=invocation):
                tools = FakeTools(fail_host_at=invocation)
                with self.assertRaises(subprocess.CalledProcessError):
                    self.package(tools)
                self.assertEqual(tools.host_calls, invocation)
                self.assertPreviousPreserved()

    def test_fresh_snapshot_ignores_stale_same_version_build_and_binds_dirty_source(self):
        (self.root / "build").mkdir()
        (self.root / "build/stale.vst3").write_bytes(b"old binary with same version")
        (self.root / "Source/processor.cpp").write_text("uncommitted current source\n")
        tools = FakeTools()
        candidate = self.package(tools)
        manifest = json.loads((candidate / "release-manifest.json").read_text())
        self.assertTrue(manifest["dirty"])
        self.assertEqual(manifest["built_binary_sha256"], pipeline.digest(b"uncommitted current source\n"))
        self.assertEqual(self.previous.read_bytes(), b"keep previous release")
        self.assertFalse((candidate / "build/stale.vst3").exists())
        commands = [Path(command[0]).name for command in tools.commands]
        self.assertEqual(tools.commands[0][0], sys.executable)
        self.assertEqual(tools.commands[0][1], "-I")
        self.assertEqual(Path(tools.commands[0][2]).name, "generate-presets.py")
        self.assertEqual(tools.commands[0][3:], ["--check"])
        self.assertLess(commands.index("ctest"), commands.index("pkgbuild"))
        self.assertEqual(commands.count("SubLab808HostTests"), 2)
        self.assertTrue((candidate / "ctest-results.xml").is_file())
        self.assertTrue((candidate / "CTest-LastTest.log").is_file())
        for name, expected in manifest["artifacts"].items():
            self.assertEqual(pipeline.file_hash(candidate / name), expected)

    def test_source_hash_changes_for_uncommitted_edit_and_new_source_file(self):
        _, first = pipeline.source_inputs(self.root)
        (self.root / "Source/processor.cpp").write_text("changed\n")
        _, second = pipeline.source_inputs(self.root)
        (self.root / "Tests").mkdir()
        (self.root / "Tests/new_test.cpp").write_text("new test\n")
        _, third = pipeline.source_inputs(self.root)
        self.assertEqual(first["repositories"][0]["commit"], third["repositories"][0]["commit"])
        self.assertNotEqual(first["source_sha256"], second["source_sha256"])
        self.assertNotEqual(second["source_sha256"], third["source_sha256"])
        self.assertIn("Tests/new_test.cpp", [record["path"] for record in third["files"]])
        (self.root / "Presets").mkdir(exist_ok=True)
        (self.root / "Presets/new-preset.json").write_text('{"name":"new"}\n')
        _, fourth = pipeline.source_inputs(self.root)
        self.assertNotEqual(third["source_sha256"], fourth["source_sha256"])
        self.assertIn("Presets/new-preset.json", [record["path"] for record in fourth["files"]])

    def test_existing_candidate_is_not_replaced(self):
        candidate = self.package()
        before = (candidate / "release-manifest.json").read_bytes()
        with self.assertRaisesRegex(pipeline.ReleaseError, "already exists"):
            self.package()
        self.assertEqual((candidate / "release-manifest.json").read_bytes(), before)
        self.assertEqual(self.previous.read_bytes(), b"keep previous release")

    def test_submodule_checked_out_bytes_are_part_of_source_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            module = Path(directory)
            (module / "module.cpp").write_text("original module\n")
            for command in (["git", "init", "-q"], ["git", "add", "."],
                            ["git", "-c", "user.name=Pipeline Test", "-c", "user.email=pipeline@example.invalid",
                             "commit", "-qm", "module fixture"]):
                subprocess.run(command, cwd=module, check=True, capture_output=True)
            subprocess.run(["git", "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                            str(module), "external/JUCE"], cwd=self.root, check=True, capture_output=True)
        _, before = pipeline.source_inputs(self.root)
        (self.root / "external/JUCE/module.cpp").write_text("dirty dependency\n")
        _, after = pipeline.source_inputs(self.root)
        self.assertNotEqual(before["source_sha256"], after["source_sha256"])
        self.assertEqual(after["repositories"][1]["path"], "external/JUCE")
        self.assertTrue(after["repositories"][1]["dirty"])
        self.assertIn("external/JUCE/module.cpp", [record["path"] for record in after["files"]])

    def test_atomic_publication_failure_keeps_existing_releases(self):
        artifacts = self.root / "completed-artifacts"
        artifacts.mkdir()
        (artifacts / "new.pkg").write_bytes(b"new release")
        with patch.object(Path, "rename", side_effect=OSError("simulated rename failure")):
            with self.assertRaises(OSError):
                pipeline.publish_candidate(artifacts, self.root / "dist", "new-candidate")
        self.assertPreviousPreserved()
        self.assertEqual((artifacts / "new.pkg").read_bytes(), b"new release")


if __name__ == "__main__":
    unittest.main()
