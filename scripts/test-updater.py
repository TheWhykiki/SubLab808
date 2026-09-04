#!/usr/bin/env python3
"""Build and run isolated Swift updater-policy tests on macOS."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="whykiki-updater-tests-") as temporary:
    directory = Path(temporary)
    binary = directory / "UpdaterTests"
    subprocess.run(["xcrun", "swiftc", "-module-cache-path", str(directory / "modules"),
                    str(root / "Updater/UpdateCore.swift"), str(root / "Updater/PackageService.swift"),
                    str(root / "Tests/Updater/UpdaterTests.swift"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
