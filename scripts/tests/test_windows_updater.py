import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
UPDATER = ROOT / "Updater" / "Windows"
TESTS = ROOT / "Tests" / "WindowsUpdater"


class WindowsUpdaterContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (UPDATER / "WindowsUpdater.cpp").read_text(encoding="utf-8")
        cls.policy = (UPDATER / "UpdaterPolicy.cpp").read_text(encoding="utf-8")
        cls.header = (UPDATER / "UpdaterPolicy.h").read_text(encoding="utf-8")

    def test_portable_policy_executes(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("No C++ compiler is available for the portable policy test")
        with tempfile.TemporaryDirectory(prefix="windows-updater-policy-") as temporary:
            executable = pathlib.Path(temporary) / ("policy.exe" if os.name == "nt" else "policy")
            command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       "-I", str(UPDATER), str(UPDATER / "UpdaterPolicy.cpp"),
                       str(TESTS / "PolicyTests.cpp"), "-o", str(executable)]
            compiled = subprocess.run(command, text=True, capture_output=True, timeout=60, check=False)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True,
                                    timeout=10, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS: Windows updater portable policy", result.stdout)

    def test_production_build_requires_real_signer_pin(self):
        self.assertIn("#if ! WK_WINDOWS_UPDATER_TEST_MODE && ! defined(WK_WINDOWS_UPDATER_SIGNER_SHA256)",
                      self.source)
        self.assertIn("static_assert(kTestMode || compileTimePinIsValid()", self.source)
        self.assertIn("Test mode cannot launch Windows Installer or request elevation", self.source)
        self.assertLess(
            self.source.index('require(! kTestMode, "Test mode cannot launch Windows Installer'),
            self.source.index("ShellExecuteExW(&launch)"),
        )

    def test_network_release_and_digest_contract_is_bounded(self):
        required = [
            "WINHTTP_DISABLE_REDIRECTS", "kMaximumRedirects", "kMaximumMetadataBytes",
            "kMaximumMsiBytes", "std::chrono::minutes(10)", "isAllowedHttpsHost",
            "browser_download_url", 'getProperty("digest")', "constantTimeEqual",
            "expectedAssetUrl(kOwner, kRepository, kProduct", "releasesApiUrl(kOwner, kRepository)",
        ]
        for token in required:
            self.assertIn(token, self.source)
        self.assertNotIn("Authorization:", self.source)
        self.assertNotIn("api.github.com/repos/" + "${", self.source)

    def test_msi_and_payload_are_checked_before_elevation(self):
        implementation = self.source + "\n" + self.policy
        required = [
            "WinVerifyTrust", "CERT_SHA256_HASH_PROP_ID", "verifyMsiDatabase",
            "isForbiddenMsiSideEffectTable", "hasExactUpgradeContract",
            "hasExactLaunchConditions", "componentDirectoriesAreInsideInstallFolder",
            '"ForceReboot"', '"ScheduleReboot"', '"DisableRollback"',
            "component directory does not descend from INSTALLFOLDER",
            "MSI File row refers to an unknown payload component",
            "MSI Upgrade table is not the exact three-row architecture/downgrade contract",
            "Administrative image contains a file outside the single VST3 payload",
            "fingerprintBundle", "CHPEMetadataPointer", "hasNamedDataStream",
            'property("MSIDEPLOYMENTCOMPLIANT") == "1"',
            'property("SecureCustomProperties")',
            "MSI SecureCustomProperties is not the exact upgrade-detection set",
        ]
        for token in required:
            self.assertIn(token, implementation)
        policy_test = (TESTS / "PolicyTests.cpp").read_text(encoding="utf-8")
        for mutation_gate in ("upgrade attribute mutant rejected",
                              "non-NULL WiX Upgrade Remove policy rejected",
                              "component escaping to WindowsFolder rejected",
                              "disconnected directory cycle rejected",
                              "dangling directory parent rejected",
                              "dangerous MSI side-effect table policy"):
            self.assertIn(mutation_gate, policy_test)
        launch = self.source.index("ShellExecuteExW(&launch)")
        self.assertLess(self.source.index("verifyDownloadedMsi(msi, journal)",
                                          self.source.index("DWORD launchMsi")), launch)
        self.assertIn("ERROR_SUCCESS_REBOOT_REQUIRED", self.source)

    def test_elevated_installer_never_writes_to_a_user_controlled_log_path(self):
        start = self.source.index("DWORD installMsi(")
        end = self.source.index("Path currentExecutable()", start)
        elevated_install = self.source[start:end]
        self.assertIn('const auto arguments = L"/i " + quote(msi);', elevated_install)
        self.assertNotIn("/L*V", elevated_install)
        self.assertNotIn("install.log", elevated_install)

        # The unelevated administrative extraction may retain its diagnostic
        # log because it cannot write with privileges the caller does not own.
        extraction_start = self.source.index("void administrativeExtract(")
        extraction = self.source[extraction_start:start]
        self.assertIn("/L*V", extraction)

    def test_operation_is_on_demand_private_and_resumable(self):
        required = [
            "--resume", "journal.json", "MoveFileExW", "MOVEFILE_WRITE_THROUGH",
            "ConvertStringSecurityDescriptorToSecurityDescriptorW", "operation.lock",
            "int worker(const Path& operation, Journal& journal)",
            "CopyFileW(self.c_str(), copied.c_str()", "Another updater operation is active",
            "Der Updater beendet keine Programme", "fragt niemals selbst nach einem Passwort",
        ]
        for token in required:
            self.assertIn(token, self.source)
        forbidden = ["CreateServiceW", "RegSetValue", "schtasks", "TerminateProcess",
                     "OpenProcess(PROCESS_TERMINATE", "taskschd"]
        for token in forbidden:
            self.assertNotIn(token, self.source)

    def test_copied_updater_is_locked_and_reverified_until_process_start(self):
        start = self.source.index("void launchCopiedUpdater(")
        end = self.source.index("class ProductMutex", start)
        launch = self.source[start:end]
        for token in (
            "FILE_FLAG_OPEN_REPARSE_POINT",
            "FILE_SHARE_READ",
            "GetFileInformationByHandle",
            "verifyAuthenticode(executable)",
            "hashFile(source) == hashFile(executable)",
            "ShellExecuteExW(&launch)",
        ):
            self.assertIn(token, launch)
        self.assertNotIn("FILE_SHARE_WRITE", launch)
        self.assertNotIn("FILE_SHARE_DELETE", launch)
        self.assertLess(launch.index("verifyAuthenticode(executable)"),
                        launch.index("ShellExecuteExW(&launch)"))

    def test_build_contract_validation_is_side_effect_free_and_compile_only_is_closed(self):
        required = [
            "--validate-build-contract", "--challenge", "--response-pipe",
            "--parent-process-id", "--product", "--version", "--manufacturer",
            "--github-owner", "--github-repository", "--architecture",
            "--upgrade-code", "--other-upgrade-code", "--signer-sha256",
            "buildContractMatches", "WK_WINDOWS_UPDATER_COMPILE_ONLY",
            "whykiki.windows-updater-build-contract", "canonicalBuildContractResponse",
            "GetNamedPipeServerProcessId", "SECURITY_SQOS_PRESENT",
        ]
        for token in required:
            self.assertIn(token, self.source)
        self.assertIn("return ! kTestMode && ! kCompileOnly", self.source)
        self.assertIn("Test-mode executable was allowed to certify a distribution build", self.source)
        self.assertIn("Build-contract manufacturer mutation was accepted", self.source)
        self.assertIn("Build-contract owner mutation was accepted", self.source)
        self.assertIn("Build-contract repository mutation was accepted", self.source)
        self.assertIn("Build-contract challenge validation failed", self.source)
        self.assertIn("Build-contract pipe-name validation failed", self.source)
        dispatch = self.source.index(
            "if (const auto contractStatus = validateBuildContractCommandLine())")
        compile_only = self.source.index("if (kCompileOnly) return 4;", dispatch)
        normal_setup = self.source.index("ProductMutex mutex;", compile_only)
        self.assertLess(dispatch, compile_only)
        self.assertLess(compile_only, normal_setup)
        validator_start = self.source.index("std::optional<int> validateBuildContractCommandLine()")
        validator_end = self.source.index("Invocation invocation()", validator_start)
        validator = self.source[validator_start:validator_end]
        for forbidden in ("taskDialog(", "httpGet(", "CreateFileW(", "ShellExecuteExW("):
            self.assertNotIn(forbidden, validator)
        match = validator.index("buildContractMatches(")
        response = validator.index("writeBuildContractResponse(")
        self.assertLess(match, response)
        response_start = self.source.index("void writeBuildContractResponse(")
        response_end = self.source.index("std::optional<int> validateBuildContractCommandLine()",
                                         response_start)
        response_writer = self.source[response_start:response_end]
        self.assertIn(r'L"\\\\.\\pipe\\"', response_writer)
        self.assertIn("response.size() <= 4096", response_writer)
        self.assertNotIn("GetStdHandle", response_writer)

    def test_semver_matches_windows_installer_limits(self):
        self.assertIn("*major > 255 || *minor > 255 || *patch > 65535", self.policy)
        policy_test = (TESTS / "PolicyTests.cpp").read_text(encoding="utf-8")
        for boundary in ("255.255.65535", "256.0.0", "0.256.0", "0.0.65536"):
            self.assertIn(boundary, policy_test)


if __name__ == "__main__":
    unittest.main()
