import Foundation
import CryptoKit
import Darwin

struct ToolOutput {
    let status: Int32
    let text: String
}

func runTool(_ executable: String, _ arguments: [String], timeout: TimeInterval = 60) throws -> ToolOutput {
    let log = FileManager.default.temporaryDirectory.appendingPathComponent("whykiki-tool-" + UUID().uuidString)
    FileManager.default.createFile(atPath: log.path, contents: nil, attributes: [.posixPermissions: 0o600])
    let handle = try FileHandle(forWritingTo: log)
    defer { try? handle.close(); try? FileManager.default.removeItem(at: log) }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: executable)
    process.arguments = arguments
    process.environment = ["PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C"]
    process.standardOutput = handle
    process.standardError = handle
    try process.run()
    let deadline = Date().addingTimeInterval(timeout)
    while process.isRunning && Date() < deadline { Thread.sleep(forTimeInterval: 0.05) }
    if process.isRunning {
        process.terminate()
        Thread.sleep(forTimeInterval: 0.2)
        if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        process.waitUntilExit()
        throw UpdateFailure("macOS hat die Paketprüfung nicht rechtzeitig abgeschlossen")
    }
    process.waitUntilExit()
    return ToolOutput(status: process.terminationStatus,
                      text: String(data: try Data(contentsOf: log), encoding: .utf8) ?? "")
}

func verifiedTool(_ executable: String, _ arguments: [String], message: String) throws {
    let result = try runTool(executable, arguments)
    try require(result.status == 0, message + "\n" + String(result.text.prefix(1200)))
}

func rejectSymlinkAncestors(_ url: URL) throws {
    var current = url.standardizedFileURL
    while current.path != "/" {
        if let attributes = try? FileManager.default.attributesOfItem(atPath: current.path) {
            // Foundation canonicalises Apple's /private aliases back to /var and /tmp.
            // Permit only these exact system aliases, never arbitrary user symlinks.
            if (current.path == "/var" || current.path == "/tmp"),
               let target = try? FileManager.default.destinationOfSymbolicLink(atPath: current.path),
               target == "/private" + current.path || target == "private" + current.path {
                current.deleteLastPathComponent()
                continue
            }
            try require(attributes[.type] as? FileAttributeType != .typeSymbolicLink, "Ein Installationspfad ist ein symbolischer Link: \(current.path)")
        }
        current.deleteLastPathComponent()
    }
}

func bundleFingerprint(_ bundle: URL) throws -> [String: String] {
    try rejectSymlinkAncestors(bundle)
    var enumerationError: Error?
    guard let enumerator = FileManager.default.enumerator(at: bundle, includingPropertiesForKeys: [.isRegularFileKey, .isDirectoryKey, .isSymbolicLinkKey],
                                                         options: [], errorHandler: { _, error in enumerationError = error; return false }) else {
        throw UpdateFailure("Plugin-Dateien konnten nicht gelesen werden")
    }
    var result: [String: String] = [:]
    for case let url as URL in enumerator {
        let values = try url.resourceValues(forKeys: [.isRegularFileKey, .isDirectoryKey, .isSymbolicLinkKey])
        try require(values.isSymbolicLink != true, "Update-Bundles mit symbolischen Links werden nicht unterstützt")
        let relative = String(url.path.dropFirst(bundle.path.count + 1))
        if values.isDirectory == true { result[relative] = "directory"; continue }
        try require(values.isRegularFile == true, "Unbekannter Dateityp im Plugin")
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var hasher = SHA256()
        while let block = try handle.read(upToCount: 1024 * 1024), !block.isEmpty { hasher.update(data: block) }
        result[relative] = hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }
    if let error = enumerationError { throw error }
    try require(!result.isEmpty, "Leeres Plugin-Bundle")
    return result
}

struct UserCopy {
    let url: URL
    let version: Version
    let fingerprint: [String: String]

    static func capture(_ product: Product) throws -> UserCopy? {
        guard FileManager.default.fileExists(atPath: product.userBundle.path) else { return nil }
        return UserCopy(url: product.userBundle,
                        version: try validateBundleMetadata(product.userBundle, product: product),
                        fingerprint: try bundleFingerprint(product.userBundle))
    }

    func archive(to directory: URL) throws -> URL {
        try rejectSymlinkAncestors(directory)
        try require(try bundleFingerprint(url) == fingerprint, "Die bisherige Benutzerinstallation wurde zwischenzeitlich geändert. Sie bleibt erhalten.")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let destination = directory.appendingPathComponent(url.lastPathComponent)
        try require(!FileManager.default.fileExists(atPath: destination.path), "Der Sicherungsordner ist bereits belegt")
        try require(rename(url.path, destination.path) == 0, "Die bisherige Benutzerinstallation konnte nicht gesichert werden")
        do {
            try require(try bundleFingerprint(destination) == fingerprint, "Die Sicherung der bisherigen Installation hat sich verändert")
        } catch {
            if !FileManager.default.fileExists(atPath: url.path) { _ = rename(destination.path, url.path) }
            throw error
        }
        return destination
    }
}

struct PreparedPackage {
    let file: URL
    let candidate: UpdateCandidate
    let fingerprint: [String: String]
}

enum PackageService {
    static func prepare(_ package: URL, candidate: UpdateCandidate, product: Product, workspace: URL) throws -> PreparedPackage {
        try candidate.verifyDownload(package)
        try verifiedTool("/usr/sbin/pkgutil", ["--check-signature", package.path],
                         message: "Das Paket hat keine gültige vertrauenswürdige Installer-Signatur.")
        try verifiedTool("/usr/sbin/spctl", ["--assess", "--type", "install", "--verbose=2", package.path],
                         message: "macOS hat dieses Installationspaket nicht freigegeben. Es wird nicht automatisch installiert.")
        let expanded = workspace.appendingPathComponent("expanded-" + UUID().uuidString, isDirectory: true)
        try verifiedTool("/usr/sbin/pkgutil", ["--expand-full", package.path, expanded.path], message: "Paket konnte nicht geprüft werden")
        try PackageMetadata.validate(Data(contentsOf: expanded.appendingPathComponent("PackageInfo")), product: product, version: candidate.version)
        try require(!FileManager.default.fileExists(atPath: expanded.appendingPathComponent("Scripts").path), "Installationsskripte sind in Plugin-Updates nicht erlaubt")
        let payload = expanded.appendingPathComponent("Payload", isDirectory: true)
        let expected = payload.appendingPathComponent("Library/Audio/Plug-Ins/VST3/" + product.rawValue + ".vst3", isDirectory: true)
        var enumerationError: Error?
        guard let entries = FileManager.default.enumerator(at: payload, includingPropertiesForKeys: [.isDirectoryKey],
                                                           errorHandler: { _, error in enumerationError = error; return false }) else { throw UpdateFailure("Paketinhalt fehlt") }
        let prefix = "Library/Audio/Plug-Ins/VST3/" + product.rawValue + ".vst3"
        let parents: Set<String> = ["Library", "Library/Audio", "Library/Audio/Plug-Ins", "Library/Audio/Plug-Ins/VST3"]
        for case let entry as URL in entries {
            let relative = String(entry.path.dropFirst(payload.path.count + 1))
            try require(parents.contains(relative) || relative == prefix || relative.hasPrefix(prefix + "/"), "Das Paket enthält Dateien außerhalb des erwarteten Plugins")
        }
        if let error = enumerationError { throw error }
        _ = try validateBundleMetadata(expected, product: product, version: candidate.version)
        let binary = try FileHandle(forReadingFrom: expected.appendingPathComponent("Contents/MacOS/" + product.rawValue))
        defer { try? binary.close() }
        try product.validateMachO(binary.read(upToCount: 4096) ?? Data())
        try verifiedTool("/usr/bin/codesign", ["--verify", "--deep", "--strict", expected.path], message: "Plugin-Signatur ist beschädigt")
        let fingerprint = try bundleFingerprint(expected)
        try candidate.verifyDownload(package) // The assessed bytes are also the bytes handed to Installer.
        return PreparedPackage(file: package, candidate: candidate, fingerprint: fingerprint)
    }

    static func installed(_ prepared: PreparedPackage, product: Product) throws -> Bool {
        guard (try? validateBundleMetadata(product.systemBundle, product: product, version: prepared.candidate.version)) != nil else { return false }
        let receipt = try runTool("/usr/sbin/pkgutil", ["--pkg-info-plist", product.packageID], timeout: 10)
        guard receipt.status == 0, let data = receipt.text.data(using: .utf8),
              let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              info["pkg-version"] as? String == prepared.candidate.version.description else { return false }
        try require(try bundleFingerprint(product.systemBundle) == prepared.fingerprint, "Die installierten Plugin-Dateien stimmen nicht mit dem geprüften Paket überein")
        try verifiedTool("/usr/bin/codesign", ["--verify", "--deep", "--strict", product.systemBundle.path], message: "Die installierte Plugin-Signatur ist ungültig")
        return true
    }

    static func usersOfPlugin(_ product: Product) throws -> Set<Int32> {
        var pids = Set<Int32>()
        for bundle in [product.userBundle, product.systemBundle] {
            let binary = bundle.appendingPathComponent("Contents/MacOS/" + product.rawValue)
            guard FileManager.default.fileExists(atPath: binary.path) else { continue }
            let result = try runTool("/usr/sbin/lsof", ["-t", "--", binary.path], timeout: 10)
            try require(result.status == 0 || (result.status == 1 && result.text.isEmpty), "Laufende Plugin-Nutzung konnte nicht sicher geprüft werden")
            for line in result.text.split(whereSeparator: \.isNewline) {
                guard let pid = Int32(line) else { throw UpdateFailure("Unerwartete Antwort bei der DAW-Prüfung") }
                pids.insert(pid)
            }
        }
        return pids
    }

    static func rejectDowngrade(_ product: Product, target: Version) throws {
        for bundle in [product.userBundle, product.systemBundle] {
            try rejectSymlinkAncestors(bundle)
            guard FileManager.default.fileExists(atPath: bundle.path) else { continue }
            try require(try validateBundleMetadata(bundle, product: product) <= target,
                        "Eine vorhandene Installation ist neuer als dieses Update. Sie wird nicht ersetzt.")
        }
    }
}
