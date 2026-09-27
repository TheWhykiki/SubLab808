#!/usr/bin/env python3
"""Verify a macOS PKG and universal VST3 against the production updater trust policy."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--application-cert-sha256", required=True)
    parser.add_argument("--installer-cert-sha256", required=True)
    parser.add_argument("--next-application-cert-sha256", default="")
    parser.add_argument("--next-installer-cert-sha256", default="")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="whykiki-signer-verifier-") as temporary:
        stage = Path(temporary)
        main_source = stage / "Verifier.swift"
        main_source.write_text('''import Foundation
import Darwin
@main struct Verifier {
    static func main() {
        do { try verify() }
        catch {
            FileHandle.standardError.write(Data((error.localizedDescription + "\\n").utf8))
            exit(1)
        }
    }
    static func verify() throws {
        let args = CommandLine.arguments
        try require(args.count == 7, "Invalid verifier arguments")
        let policy = try SignerPolicy(application: args[3], installer: args[4], nextApplication: args[5], nextInstaller: args[6])
        try policy.requireConfigured()
        let package = URL(fileURLWithPath: args[1]), bundle = URL(fileURLWithPath: args[2])
        try rejectSymlinkAncestors(package)
        try rejectSymlinkAncestors(bundle)
        let installer = try PackageService.installerSHA256(package)
        let slices = try ApplicationSignature.sliceSHA256(bundle)
        try policy.validate(installer: installer, applicationSlices: slices)
        let report: [String: Any] = ["installerSHA256": installer, "applicationSlices": slices, "verified": true]
        let data = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
        print(String(data: data, encoding: .utf8)!)
    }
}
''')
        binary = stage / "verify-signers"
        sources = [root / f"Updater/{name}.swift" for name in ("UpdateCore", "PackageService", "SignerPolicy")]
        subprocess.run(["xcrun", "swiftc", "-module-cache-path", str(stage / "modules"),
                        *map(str, sources), str(main_source), "-o", str(binary)], check=True)
        subprocess.run([str(binary), str(args.package), str(args.bundle), args.application_cert_sha256,
                        args.installer_cert_sha256, args.next_application_cert_sha256,
                        args.next_installer_cert_sha256], check=True)


if __name__ == "__main__":
    main()
