#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Rebuild and load an unsigned VST3 using only the distributed source ZIP."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

from release_contract import load_product
from source_release import create_archive, verify_archive, verify_tree


def run(arguments, cwd=None):
    subprocess.run([str(value) for value in arguments], cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--juce", type=Path, required=True)
    parser.add_argument("--generator", default="Unix Makefiles")
    parser.add_argument("--platform")
    args = parser.parse_args()
    config = load_product(args.repo)
    product = config["productName"]
    with tempfile.TemporaryDirectory(prefix="whykiki-source-rebuild-") as temp:
        directory = Path(temp)
        archive = directory / "source.zip"
        create_archive(args.repo, args.juce, archive)
        verify_archive(archive)
        with zipfile.ZipFile(archive) as source:
            source.extractall(directory)
            for entry in source.infolist():
                os.chmod(directory / entry.filename, (entry.external_attr >> 16) & 0o777)
        root = directory / f"{product}-{config['version']}-Source"
        verify_tree(root)
        build = directory / "build"
        configure = ["cmake", "-S", root, "-B", build, "-G", args.generator, "-DCMAKE_BUILD_TYPE=Release"]
        if args.platform:
            configure += ["-A", args.platform]
        run(configure)
        run(["cmake", "--build", build, "--config", "Release", "--target",
             f"{product}_VST3", f"{product}HostTests", "--parallel", "4"])
        run(["ctest", "--test-dir", build, "-C", "Release", "--output-on-failure",
             "--no-tests=error", "-R", f"^{product}BundleLoad$"])
    print(f"{product}: clean source-only VST3 rebuild and host load passed")


if __name__ == "__main__":
    main()
