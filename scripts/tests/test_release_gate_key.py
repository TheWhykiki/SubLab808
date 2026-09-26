# SPDX-License-Identifier: AGPL-3.0-only
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ReleaseGateKeyTests(unittest.TestCase):
    def test_mocked_azure_cli_contract(self):
        pwsh = os.environ.get("PWSH") or shutil.which("pwsh")
        if not pwsh:
            self.skipTest("PowerShell is required for mocked Azure Key Vault tests")
        result = subprocess.run(
            [pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File",
             str(ROOT / "scripts/tests/test_release_gate_key.ps1")],
            capture_output=True, text=True, timeout=30, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: Key Vault CLI encoding", result.stdout)
