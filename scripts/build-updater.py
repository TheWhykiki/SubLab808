#!/usr/bin/env python3
"""Build the dependency-free native updater app; no install or network activity."""
import argparse
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", choices=("SubLab808", "ReverseLab"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), action="append", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error("version must have three numeric components")
    root = Path(__file__).resolve().parent.parent
    destination = args.output.resolve()
    if destination.name != args.product + "Updater.app":
        parser.error("output must name the product's generated Updater.app bundle")
    destination.parent.mkdir(parents=True, exist_ok=True)
    sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="updater-build-", dir=destination.parent) as temporary:
        stage = Path(temporary)
        app = stage / destination.name
        binaries = app / "Contents/MacOS"
        binaries.mkdir(parents=True)
        name = args.product + "Updater"
        slices = []
        for architecture in dict.fromkeys(args.arch):
            binary = stage / (name + "-" + architecture)
            subprocess.run(["xcrun", "swiftc", "-O", "-module-cache-path", str(stage / "modules"),
                            "-sdk", sdk, "-target", architecture + "-apple-macosx11.0",
                            *map(str, sorted((root / "Updater").glob("*.swift"))), "-o", str(binary)], check=True)
            slices.append(str(binary))
        subprocess.run(["xcrun", "lipo", "-create", *slices, "-output", str(binaries / name)], check=True)
        info = {"CFBundleIdentifier": "audio.whykiki." + args.product.lower() + ".updater",
                "CFBundleName": args.product + " Update", "CFBundleExecutable": name,
                "CFBundlePackageType": "APPL", "CFBundleShortVersionString": args.version,
                "CFBundleVersion": args.version, "LSMinimumSystemVersion": "11.0",
                "NSHighResolutionCapable": True, "WKProduct": args.product}
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
        subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(str(app), str(destination))


if __name__ == "__main__":
    main()
