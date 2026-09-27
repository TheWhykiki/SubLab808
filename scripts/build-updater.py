#!/usr/bin/env python3
"""Build the dependency-free native updater app; no install or network activity."""
import argparse
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile


def validate_pins(values):
    normalized = []
    for value in values:
        if value and not re.fullmatch(r"[0-9A-Fa-f]{64}", value):
            raise ValueError("certificate SHA-256 must be empty or exactly 64 hexadecimal characters")
        normalized.append(value.lower())
    application, installer, next_application, next_installer = normalized
    if bool(application) != bool(installer) or bool(next_application) != bool(next_installer):
        raise ValueError("application and installer certificate pins must be complete pairs")
    if next_application and not application:
        raise ValueError("next certificate pair requires the current pair")
    if next_application and (application, installer) == (next_application, next_installer):
        raise ValueError("next certificate pair must differ from current")
    return normalized


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", choices=("SubLab808", "ReverseLab"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), action="append", required=True)
    parser.add_argument("--application-cert-sha256", default="")
    parser.add_argument("--installer-cert-sha256", default="")
    parser.add_argument("--next-application-cert-sha256", default="")
    parser.add_argument("--next-installer-cert-sha256", default="")
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        parser.error("version must have three numeric components")
    try:
        pins = validate_pins([args.application_cert_sha256, args.installer_cert_sha256,
                              args.next_application_cert_sha256, args.next_installer_cert_sha256])
    except ValueError as error:
        parser.error(str(error))
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
        configuration = stage / "EmbeddedSignerConfiguration.swift"
        fields = ("currentApplication", "currentInstaller", "nextApplication", "nextInstaller")
        configuration.write_text("enum EmbeddedSignerConfiguration {\n" + "".join(
            f"    static let {field} = {json.dumps(value)}\n" for field, value in zip(fields, pins)) + "}\n")
        slices = []
        for architecture in dict.fromkeys(args.arch):
            binary = stage / (name + "-" + architecture)
            subprocess.run(["xcrun", "swiftc", "-O", "-module-cache-path", str(stage / "modules"),
                            "-sdk", sdk, "-target", architecture + "-apple-macosx11.0",
                            "-D", "WK_CONFIGURED_SIGNERS", str(configuration),
                            *map(str, sorted((root / "Updater").glob("*.swift"))), "-o", str(binary)], check=True)
            slices.append(str(binary))
        subprocess.run(["xcrun", "lipo", "-create", *slices, "-output", str(binaries / name)], check=True)
        info = {"CFBundleIdentifier": "audio.whykiki." + args.product.lower() + ".updater",
                "CFBundleName": args.product + " Update", "CFBundleExecutable": name,
                "CFBundlePackageType": "APPL", "CFBundleShortVersionString": args.version,
                "CFBundleVersion": args.version, "LSMinimumSystemVersion": "11.0",
                "NSHighResolutionCapable": True, "WKProduct": args.product,
                "WKSignerPins": dict(zip(fields, pins)), "WKLicense": "AGPL-3.0-only",
                "WKSourceURL": f"https://github.com/TheWhykiki/{args.product}/tree/v{args.version}"}
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
        subprocess.run(["codesign", "--force", "--sign", "-", str(app)], check=True)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], check=True)
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(str(app), str(destination))


if __name__ == "__main__":
    main()
