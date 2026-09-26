import Foundation

@main struct SignerPolicyTests {
    static var count = 0
    static let app = String(repeating: "a", count: 64), installer = String(repeating: "b", count: 64)
    static let nextApp = String(repeating: "c", count: 64), nextInstaller = String(repeating: "d", count: 64)
    static func test(_ name: String, _ body: () throws -> Void) throws {
        try body(); count += 1; print("PASS " + name)
    }
    static func reject(_ body: () throws -> Void) throws {
        do { try body() } catch { return }; throw UpdateFailure("Expected rejection")
    }
    static func output(_ hash: String = installer) -> String {
        func spaced(_ value: String) -> String { stride(from: 0, to: value.count, by: 2).map {
            String(value.dropFirst($0).prefix(2)).uppercased()
        }.joined(separator: " ") }
        return """
        Package "Example.pkg":
           Status: signed by a developer certificate issued by Apple for distribution
           Signed with a trusted timestamp on: 2026-09-26 10:00:00 +0000
           Certificate Chain:
            1. Developer ID Installer: Example (A1B2C3D4E5)
               Expires: 2028-01-01 00:00:00 +0000
               SHA256 Fingerprint:
                   \(spaced(hash))
               ------------------------------------------------------------------------
            2. Developer ID Certification Authority
               Expires: 2030-01-01 00:00:00 +0000
               SHA256 Fingerprint:
                   \(spaced(app))
        """
    }
    static func main() throws {
        let policy = try SignerPolicy(application: app, installer: installer, nextApplication: nextApp, nextInstaller: nextInstaller)
        try test("current and next complete pairs accept both slices") {
            try policy.validate(installer: installer, applicationSlices: ["arm64": app, "x86_64": app])
            try policy.validate(installer: nextInstaller, applicationSlices: ["arm64": nextApp, "x86_64": nextApp])
        }
        try test("mixed pairs foreign signers and mismatched slices fail") {
            for slices in [["arm64": nextApp, "x86_64": nextApp], ["arm64": app, "x86_64": nextApp], ["arm64": app], ["arm64": installer, "x86_64": installer]] {
                try reject { try policy.validate(installer: installer, applicationSlices: slices) }
            }
            try reject { try policy.validate(installer: app, applicationSlices: ["arm64": app, "x86_64": app]) }
            try policy.validateApplication(["arm64": app, "x86_64": app])
            try reject { try policy.validateApplication(["arm64": app, "x86_64": nextApp]) }
        }
        try test("missing malformed and incomplete configurations fail closed") {
            try reject { try SignerPolicy(application: "", installer: "").requireConfigured() }
            try reject { _ = try SignerPolicy(application: app, installer: "") }
            try reject { _ = try SignerPolicy(application: "g" + app.dropFirst(), installer: installer) }
            try reject { _ = try SignerPolicy(application: "", installer: "", nextApplication: nextApp, nextInstaller: nextInstaller) }
            try reject { _ = try SignerPolicy(application: app, installer: installer, nextApplication: nextApp) }
            try reject { _ = try SignerPolicy(application: app, installer: installer, nextApplication: app, nextInstaller: installer) }
            try reject { try SignerPolicy.embedded().requireConfigured() }
        }
        try test("pkgutil parser extracts only numbered leaf fingerprint") {
            try require(try InstallerSignature.leafSHA256(output()) == installer, "Wrong leaf fingerprint")
            let multiline = output().replacingOccurrences(of: String(repeating: "BB ", count: 16), with: String(repeating: "BB ", count: 15) + "BB\n           ")
            try require(try InstallerSignature.leafSHA256(multiline) == installer, "Wrapped fingerprint rejected")
        }
        try test("malformed duplicated and untrusted pkgutil responses fail") {
            let valid = output()
            for value in [valid + "\nSHA256 Fingerprint:\n" + installer,
                          valid.replacingOccurrences(of: "1. Developer ID Installer:", with: "1. Developer ID Application:"),
                          valid.replacingOccurrences(of: "2. Developer ID Certification Authority", with: "1. Developer ID Installer: Other (A1B2C3D4E5)"),
                          valid.replacingOccurrences(of: "Status: signed by", with: "Status: unsigned\nStatus: signed by"),
                          valid.replacingOccurrences(of: "trusted timestamp", with: "timestamp"),
                          valid.replacingOccurrences(of: "SHA256 Fingerprint:", with: "SHA256 Fingerprint:\nSHA256 Fingerprint:"),
                          valid.replacingOccurrences(of: "BB BB", with: "ZZ ZZ"),
                          valid + "\0", String(repeating: "x", count: 16385)] {
                try reject { _ = try InstallerSignature.leafSHA256(value) }
            }
        }
        try test("ad hoc executable cannot pass Developer ID application verification") {
            try reject { _ = try ApplicationSignature.sliceSHA256(URL(fileURLWithPath: CommandLine.arguments[0])) }
        }
        try test("private workspace rejects shared permissions and symlinks") {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            defer { try? FileManager.default.removeItem(at: root) }
            try requirePrivateDirectory(root)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: root.path)
            try reject { try requirePrivateDirectory(root) }
            let link = root.appendingPathComponent("link")
            try FileManager.default.createSymbolicLink(at: link, withDestinationURL: root)
            try reject { try requirePrivateDirectory(link) }
        }
        try test("tool output budget and LC_ALL apply to production tool runner") {
            let result = try runTool("/usr/bin/printenv", ["LC_ALL"], maximumOutputBytes: 8)
            try require(result.text == "C\n", "Tool locale is not deterministic")
            try reject { _ = try runTool("/usr/bin/printf", [String(repeating: "X", count: 512)], maximumOutputBytes: 32) }
        }
        print("\(count) signer policy tests passed; no production certificates or Installer were used.")
    }
}
