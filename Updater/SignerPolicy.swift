import Foundation
import CryptoKit
import Security
import Darwin

#if !WK_CONFIGURED_SIGNERS
// Local builds remain usable for UI/tests, but cannot hand a package to Installer.
enum EmbeddedSignerConfiguration {
    static let currentApplication = ""
    static let currentInstaller = ""
    static let nextApplication = ""
    static let nextInstaller = ""
}
#endif

struct SignerPair: Equatable {
    let application: String
    let installer: String
}

struct SignerPolicy {
    let pairs: [SignerPair]
    init(application: String, installer: String, nextApplication: String = "", nextInstaller: String = "") throws {
        func pair(_ application: String, _ installer: String) throws -> SignerPair? {
            if application.isEmpty && installer.isEmpty { return nil }
            for value in [application, installer] {
                try require(value.count == 64 && value.utf8.allSatisfy { (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0) },
                            "Die Signaturkonfiguration ist unvollständig oder ungültig")
            }
            return SignerPair(application: application.lowercased(), installer: installer.lowercased())
        }
        let current = try pair(application, installer)
        let next = try pair(nextApplication, nextInstaller)
        try require(next == nil || current != nil, "Die nächste Signatur benötigt ein aktuelles Zertifikatspaar")
        try require(next == nil || next != current, "Die nächste Signatur muss ein anderes Zertifikatspaar verwenden")
        pairs = [current, next].compactMap { $0 }
    }

    static func embedded() throws -> SignerPolicy {
        try SignerPolicy(application: EmbeddedSignerConfiguration.currentApplication,
                         installer: EmbeddedSignerConfiguration.currentInstaller,
                         nextApplication: EmbeddedSignerConfiguration.nextApplication,
                         nextInstaller: EmbeddedSignerConfiguration.nextInstaller)
    }

    func requireConfigured() throws {
        try require(!pairs.isEmpty, "Automatische Installation ist in diesem Entwicklungsbuild deaktiviert. Es fehlen die fest eingebetteten Herausgeberzertifikate.")
    }

    func pair(forInstaller fingerprint: String) throws -> SignerPair {
        try requireConfigured()
        let matches = pairs.filter { $0.installer == fingerprint.lowercased() }
        // A shared Installer certificate can accompany an Application rotation;
        // matching the complete pair happens after both slices have been verified.
        guard let pair = matches.first else { throw UpdateFailure("Das Installationspaket ist nicht vom erwarteten Herausgeber signiert") }
        return pair
    }

    func validate(installer: String, applicationSlices: [String: String]) throws {
        try requireConfigured()
        try require(Set(applicationSlices.keys) == Set(["arm64", "x86_64"]), "Die Signaturprüfung benötigt beide Prozessor-Architekturen")
        try require(pairs.contains { pair in pair.installer == installer.lowercased()
            && applicationSlices.values.allSatisfy { $0.lowercased() == pair.application } },
                    "Paket und beide Plugin-Architekturen müssen vom selben freigegebenen Zertifikatspaar stammen")
    }

    func validateApplication(_ slices: [String: String]) throws {
        try requireConfigured()
        try require(Set(slices.keys) == Set(["arm64", "x86_64"])
                    && pairs.contains { pair in slices.values.allSatisfy { $0.lowercased() == pair.application } },
                    "Beide Updater-Architekturen benötigen dasselbe freigegebene Application-Zertifikat")
    }
}

enum InstallerSignature {
    static let maximumOutputBytes = 16 * 1024

    // pkgutil is invoked with LC_ALL=C. Parse only its bounded, numbered certificate
    // chain, never an arbitrary fingerprint elsewhere in the output.
    static func leafSHA256(_ text: String) throws -> String {
        try require(text.utf8.count <= maximumOutputBytes && !text.contains("\0"), "Ungültige pkgutil-Signaturausgabe")
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false).map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
        try require(lines.count <= 128 && lines.allSatisfy { $0.utf8.count <= 1024 }, "Zu große pkgutil-Signaturausgabe")
        try require(lines.filter { $0 == "Status: signed by a developer certificate issued by Apple for distribution" }.count == 1,
                    "Das Paket besitzt keine vertrauenswürdige Developer-ID-Installer-Signatur")
        try require(lines.filter { $0.hasPrefix("Status:") }.count == 1
                    && lines.filter { $0.hasPrefix("Signed with a trusted timestamp on: ") && $0.count > 38 }.count == 1
                    && lines.filter { $0 == "Certificate Chain:" }.count == 1,
                    "Unvollständige pkgutil-Signaturausgabe")
        guard let chain = lines.firstIndex(of: "Certificate Chain:") else { throw UpdateFailure("Zertifikatskette fehlt") }
        let preamble = lines.prefix(chain).filter { !$0.isEmpty }
        try require(preamble.count == 3 && preamble[0].hasPrefix("Package \"") && preamble[0].hasSuffix("\":")
                    && preamble[1].hasPrefix("Status: ") && preamble[2].hasPrefix("Signed with a trusted timestamp on: "),
                    "Unerwarteter Vorspann der pkgutil-Signaturausgabe")
        var certificate = 0, readingFingerprint = false, hashes: [String] = [], fingerprint = ""
        for line in lines.dropFirst(chain + 1) {
            if line.isEmpty || line.allSatisfy({ $0 == "-" }) { continue }
            if line.range(of: "^[1-9][0-9]*\\. ", options: .regularExpression) != nil {
                if certificate > 0 {
                    try require(fingerprint.count == 64, "Unvollständiger Zertifikatsfingerabdruck")
                    hashes.append(fingerprint)
                }
                certificate += 1
                try require(certificate <= 5 && line.hasPrefix("\(certificate). "), "Mehrdeutige Zertifikatskette")
                if certificate == 1 {
                    try require(line.range(of: "^1\\. Developer ID Installer: [^\\r\\n]+ \\([A-Z0-9]{10}\\)$", options: .regularExpression) != nil,
                                "Das Blattzertifikat ist kein Developer-ID-Installer-Zertifikat")
                }
                readingFingerprint = false; fingerprint = ""
            } else if line == "SHA256 Fingerprint:" {
                try require(certificate > 0 && !readingFingerprint && fingerprint.isEmpty, "Doppelter Zertifikatsfingerabdruck")
                readingFingerprint = true
            } else if line.hasPrefix("Expires: ") {
                try require(certificate > 0 && !readingFingerprint, "Unerwartete Zertifikatsmetadaten")
            } else {
                try require(readingFingerprint && line.range(of: "^[0-9A-Fa-f]{2}( [0-9A-Fa-f]{2})*$", options: .regularExpression) != nil,
                            "Beschädigte pkgutil-Signaturausgabe")
                fingerprint += line.replacingOccurrences(of: " ", with: "").lowercased()
                try require(fingerprint.count <= 64, "Zu langer Zertifikatsfingerabdruck")
            }
        }
        try require(certificate >= 2 && fingerprint.count == 64, "Unvollständige Zertifikatskette")
        hashes.append(fingerprint)
        return hashes[0]
    }
}

enum ApplicationSignature {
    static func sliceSHA256(_ bundle: URL) throws -> [String: String] {
        var result: [String: String] = [:]
        var requirement: SecRequirement?
        let requirementText = "anchor apple generic and certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
        try require(SecRequirementCreateWithString(requirementText as CFString, [], &requirement) == errSecSuccess,
                    "Developer-ID-Signaturanforderung konnte nicht erstellt werden")
        for architecture in ["arm64", "x86_64"] {
            var code: SecStaticCode?
            let attributes = [kSecCodeAttributeArchitecture as String: architecture] as CFDictionary
            try require(SecStaticCodeCreateWithPathAndAttributes(bundle as CFURL, [], attributes, &code) == errSecSuccess,
                        "Plugin-Architektur konnte nicht für die Signaturprüfung geöffnet werden")
            guard let code = code else { throw UpdateFailure("Plugin-Signatur fehlt") }
            let flags = SecCSFlags(rawValue: kSecCSStrictValidate | kSecCSCheckNestedCode)
            try require(SecStaticCodeCheckValidity(code, flags, requirement) == errSecSuccess,
                        "Die Developer-ID-Signatur der Architektur \(architecture) ist ungültig")
            var information: CFDictionary?
            try require(SecCodeCopySigningInformation(code, SecCSFlags(rawValue: kSecCSSigningInformation), &information) == errSecSuccess,
                        "Plugin-Zertifikat konnte nicht gelesen werden")
            guard let info = information as? [String: Any],
                  let certificates = info[kSecCodeInfoCertificates as String] as? [SecCertificate],
                  let leaf = certificates.first else { throw UpdateFailure("Plugin-Blattzertifikat fehlt") }
            result[architecture] = SHA256.hash(data: SecCertificateCopyData(leaf) as Data).map { String(format: "%02x", $0) }.joined()
        }
        return result
    }
}

func requirePrivateDirectory(_ url: URL) throws {
    try rejectSymlinkAncestors(url)
    var info = stat()
    try require(lstat(url.path, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR
                && info.st_uid == getuid() && (info.st_mode & 0o077) == 0,
                "Update-Dateien benötigen einen privaten Ordner des aktuellen Benutzers")
}
