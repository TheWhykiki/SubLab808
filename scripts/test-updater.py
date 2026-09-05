#!/usr/bin/env python3
"""Compile all production updater components and run isolated tests on macOS."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
sources = sorted(str(p) for p in (root / "Updater").glob("*.swift") if p.name != "main.swift")
with tempfile.TemporaryDirectory(prefix="whykiki-updater-tests-") as temporary:
    directory = Path(temporary)
    for suite in ("UpdaterTests", "LifecycleTests", "HTTPClientTests"):
        binary = directory / suite
        subprocess.run(["xcrun", "swiftc", "-module-cache-path", str(directory / "modules"),
                        *sources, str(root / f"Tests/Updater/{suite}.swift"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=120)
