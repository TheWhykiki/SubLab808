#!/usr/bin/env python3
"""Cross-platform contracts for native Windows updater/installer integration."""

from __future__ import annotations

import json
import pathlib
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[2]


class WindowsIntegrationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cmake = (ROOT / "cmake" / "Updater.cmake").read_text(encoding="utf-8")
        cls.root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        cls.launcher = (ROOT / "Source" / "UpdaterLauncher.cpp").read_text(encoding="utf-8")
        cls.updater = (ROOT / "Updater" / "Windows" / "WindowsUpdater.cpp").read_text(
            encoding="utf-8"
        )
        cls.launcher_header = (ROOT / "Source" / "UpdaterLauncher.h").read_text(encoding="utf-8")
        cls.launcher_shape = (
            ROOT / "Tests" / "WindowsUpdater" / "LauncherLinkShape.cpp"
        ).read_text(encoding="utf-8")
        cls.main = (ROOT / "Updater" / "Windows" / "main.cpp").read_text(encoding="utf-8")
        cls.version_resource = (
            ROOT / "Updater" / "Windows" / "UpdaterVersion.rc.in"
        ).read_text(encoding="utf-8")
        cls.manifest = ET.parse(ROOT / "Updater" / "Windows" / "Updater.manifest").getroot()
        workflow_name = "build.yml" if ROOT.name == "SubLab808" else "ci.yml"
        cls.workflow = (ROOT / ".github" / "workflows" / workflow_name).read_text(encoding="utf-8")
        cls.config = json.loads(
            (ROOT / "Installer" / "Windows" / "package-config.json").read_text(encoding="utf-8")
        )

    def test_all_msvc_targets_use_the_static_runtime(self) -> None:
        runtime = 'set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")'
        self.assertEqual(self.root_cmake.count(runtime), 1)
        self.assertLess(self.root_cmake.index(runtime), self.root_cmake.index("add_subdirectory", 0)
                        if "add_subdirectory" in self.root_cmake
                        else self.root_cmake.index("FetchContent_MakeAvailable"))

    def test_windows_api_contract_is_unicode_and_uses_portable_summary_ids(self) -> None:
        self.assertIn("UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00", self.cmake)
        self.assertIn("constexpr UINT kMsiSummaryTemplate = 7u;", self.updater)
        self.assertIn("constexpr UINT kMsiSummaryRevisionNumber = 9u;", self.updater)
        self.assertGreaterEqual(self.updater.count("MsiOpenDatabaseW(path.c_str(), nullptr"), 2)
        self.assertNotIn("MSIDBOPEN_READONLY", self.updater)
        self.assertNotIn("PIDSI_TEMPLATE", self.updater)
        self.assertNotIn("PIDSI_REVNUMBER", self.updater)
        self.assertNotIn("PID_TEMPLATE", self.updater)
        self.assertNotIn("PID_REVNUMBER", self.updater)

    def test_configured_production_target_is_fail_closed(self) -> None:
        for token in (
            'file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Installer/Windows/package-config.json"',
            'WK_WINDOWS_UPDATER_GITHUB_OWNER="TheWhykiki"',
            'WK_WINDOWS_UPDATER_SIGNER_SHA256="${updater_signer}"',
            'if(updater_signer STREQUAL "")',
            'must be exactly 64 hexadecimal characters',
            'WK_UPDATER_ENABLED=1',
            'Contents/Helpers/${product}Updater.exe',
        ):
            self.assertIn(token.replace("\$", "$"), self.cmake)
        disabled = self.cmake.index('if(updater_signer STREQUAL "")')
        production = self.cmake.index("add_executable(${product}WindowsUpdater WIN32")
        enable = self.cmake.index("WK_UPDATER_ENABLED=1", production)
        self.assertLess(disabled, production)
        self.assertLess(production, enable)
        self.assertEqual(self.config["productName"], ROOT.name)
        self.assertEqual(set(self.config["upgradeCodes"]), {"x64", "arm64ec"})

    def test_ci_compiles_both_safe_and_production_shapes(self) -> None:
        for token in (
            "WindowsUpdaterSelfTests",
            "WK_WINDOWS_UPDATER_TEST_MODE=1",
            "WindowsUpdaterProductionShape",
            "WK_WINDOWS_UPDATER_COMPILE_ONLY=1",
            "Updater/Windows/Updater.manifest",
            "Updater-UNSIGNED-NOT-FOR-DISTRIBUTION",
            "WindowsUpdaterLauncherShape",
        ):
            self.assertIn(token, self.cmake)
        self.assertIn("WindowsUpdaterSelfTests", self.workflow)
        self.assertIn("WindowsUpdaterLauncherShape", self.workflow)
        self.assertIn("UNSIGNED-NOT-FOR-DISTRIBUTION", self.workflow)
        self.assertIn("build-windows-installer.ps1", self.workflow)
        self.assertIn("-AllowUnsigned", self.workflow)
        self.assertNotIn("-ExpectedSignerSha256", self.workflow)
        self.assertNotIn("UpdaterLauncherLinkShape-UNSIGNED", self.workflow)

        launcher_shape_start = self.cmake.index(
            "add_executable(${product}WindowsUpdaterLauncherShape EXCLUDE_FROM_ALL"
        )
        launcher_shape_end = self.cmake.index("string(TOUPPER", launcher_shape_start)
        launcher_shape = self.cmake[launcher_shape_start:launcher_shape_end]
        for token in (
            "Tests/WindowsUpdater/LauncherLinkShape.cpp",
            "Source/UpdaterLauncher.cpp",
            "juce::juce_gui_basics",
            "crypt32 wintrust",
            "UpdaterLauncherLinkShape-UNSIGNED-NOT-FOR-DISTRIBUTION",
        ):
            self.assertIn(token, launcher_shape)
        self.assertNotIn("WindowsUpdaterLauncherShape OBJECT", self.cmake)
        self.assertIn("wk::launchNativeUpdater", self.launcher_shape)
        self.assertIn("INVALID/LAUNCHER-LINK-SHAPE", self.launcher_shape)
        selftest_start = self.cmake.index("add_executable(${product}WindowsUpdaterSelfTests")
        selftest_end = self.cmake.index("add_test(NAME ${product}WindowsUpdaterSelfTest", selftest_start)
        self.assertIn("Updater/Windows/Updater.manifest", self.cmake[selftest_start:selftest_end])

    def test_distributed_updater_has_exact_windows_version_resource(self) -> None:
        for token in (
            "configure_file",
            "Updater/Windows/UpdaterVersion.rc.in",
            '"${updater_version_resource}" ${updater_sources}',
        ):
            self.assertIn(token, self.cmake)
        self.assertEqual(self.cmake.count('"${updater_version_resource}" ${updater_sources}'), 2)
        for token in (
            "FILEVERSION @WK_WINDOWS_UPDATER_RESOURCE_VERSION_MAJOR@",
            'VALUE "CompanyName", "@WK_WINDOWS_UPDATER_RESOURCE_MANUFACTURER@',
            'VALUE "FileDescription", "@WK_WINDOWS_UPDATER_RESOURCE_PRODUCT@ Updater',
            'VALUE "FileVersion", "@WK_WINDOWS_UPDATER_RESOURCE_VERSION@',
            'VALUE "InternalName", "@WK_WINDOWS_UPDATER_RESOURCE_PRODUCT@Updater',
            'VALUE "OriginalFilename", "@WK_WINDOWS_UPDATER_RESOURCE_PRODUCT@Updater.exe',
            'VALUE "ProductName", "@WK_WINDOWS_UPDATER_RESOURCE_PRODUCT@',
            'VALUE "ProductVersion", "@WK_WINDOWS_UPDATER_RESOURCE_VERSION@',
        ):
            self.assertIn(token, self.version_resource)

    def test_launcher_uses_loaded_bundle_and_create_process_without_shell(self) -> None:
        for token in (
            "GetModuleHandleExW",
            "GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS",
            "GetModuleFileNameW",
            'bundle.extension() != L".vst3"',
            'L"Contents" / L"Helpers"',
            "std::wstring(product.toWideCharPointer())",
            "CreateProcessW(finalHelper.c_str(), nullptr",
            "WinVerifyTrust",
            "WTD_REVOKE_NONE",
            "WTD_CACHE_ONLY_URL_RETRIEVAL",
            "WTD_DISABLE_MD2_MD4",
            "WTD_STATEACTION_CLOSE",
            "status != ERROR_SUCCESS",
            "CERT_SHA256_HASH_PROP_ID",
            "FILE_FLAG_OPEN_REPARSE_POINT",
            "GetFinalPathNameByHandleW",
            "INVALID_HANDLE_VALUE",
            "FALSE",
            "CloseHandle(process.hThread)",
            "CloseHandle(process.hProcess)",
        ):
            self.assertIn(token, self.launcher)
        self.assertNotIn("WTD_REVOKE_WHOLECHAIN", self.launcher)
        self.assertNotIn("WTD_REVOCATION_CHECK_NONE", self.launcher)
        self.assertNotIn("WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT", self.launcher)
        self.assertNotIn("WTD_SAFER_FLAG", self.launcher)
        local_trust = self.launcher.index("WTD_REVOKE_NONE")
        signer_pin = self.launcher.index("CERT_SHA256_HASH_PROP_ID")
        process_start = self.launcher.index("CreateProcessW(finalHelper.c_str(), nullptr")
        self.assertLess(local_trust, signer_pin)
        self.assertLess(signer_pin, process_start)
        for token in (
            "trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN",
            "WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_SAFER_FLAG",
        ):
            self.assertIn(token, self.updater)
        self.assertNotIn("WTD_CACHE_ONLY_URL_RETRIEVAL", self.updater)
        self.assertNotIn("ShellExecute", self.launcher)
        self.assertNotIn("system(", self.launcher)
        self.assertNotIn("toStdWString", self.launcher)
        self.assertNotIn("#define NOMINMAX", self.launcher)
        for token in (
            "class UpdaterButtonState final",
            "juce::ComponentMovementWatcher",
            "juce::Component::SafePointer<juce::TextButton>",
            "juce::ScopedMessageBox message",
            "juce::AlertWindow::showScopedAsync",
            ".withAssociatedComponent(button.getComponent())",
            "if (! state.ownerIsShowing()) state.closeMessage()",
        ):
            self.assertIn(token, self.launcher_header)
        self.assertNotIn("showMessageBoxAsync", self.launcher_header)
        self.assertIn("JUCE_WINDOWS", self.launcher_header)
        self.assertIn("Automatic updates require a signed Windows release build", self.launcher_header)

    def test_updater_manifest_is_as_invoker_with_common_controls_v6(self) -> None:
        nodes = list(self.manifest.iter())
        requested = [node for node in nodes if node.tag.endswith("requestedExecutionLevel")]
        dependencies = [
            node
            for node in nodes
            if node.tag.endswith("assemblyIdentity")
            and node.attrib.get("name") == "Microsoft.Windows.Common-Controls"
        ]
        self.assertEqual(len(requested), 1)
        self.assertEqual(requested[0].attrib, {"level": "asInvoker", "uiAccess": "false"})
        self.assertEqual(len(dependencies), 1)
        self.assertEqual(dependencies[0].attrib.get("version"), "6.0.0.0")
        self.assertEqual(dependencies[0].attrib.get("processorArchitecture"), "*")
        self.assertIn("InitCommonControlsEx", self.main)


if __name__ == "__main__":
    unittest.main()
