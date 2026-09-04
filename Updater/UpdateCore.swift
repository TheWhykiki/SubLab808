import Foundation
import CryptoKit

struct UpdateFailure: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

func require(_ condition: Bool, _ message: String) throws {
    if !condition { throw UpdateFailure(message) }
}

struct Version: Comparable, CustomStringConvertible {
    let parts: [Int]
    init(_ text: String) throws {
        let number = text.hasPrefix("v") ? String(text.dropFirst()) : text
        let fields = number.split(separator: ".", omittingEmptySubsequences: false)
        try require(fields.count == 3, "Ungültige Versionsnummer: \(text)")
        parts = try fields.map { field in
            try require(!field.isEmpty && field.allSatisfy { $0 >= "0" && $0 <= "9" }
                        && (field.count == 1 || field.first != "0"), "Ungültige Versionsnummer: \(text)")
            guard let value = Int(field), value <= 999999 else { throw UpdateFailure("Versionsnummer außerhalb des Bereichs") }
            return value
        }
    }
    var description: String { parts.map(String.init).joined(separator: ".") }
    static func < (lhs: Version, rhs: Version) -> Bool { lhs.parts.lexicographicallyPrecedes(rhs.parts) }
}

enum Product: String, CaseIterable {
    case sublab = "SubLab808", reverse = "ReverseLab"
    var bundleID: String { "audio.whykiki." + (self == .sublab ? "sublab808" : "reverselab") }
    var packageID: String { bundleID + ".pkg" }
    var architecture: String { self == .sublab ? "arm64" : "universal" }
    func validateArchitectures(_ value: String) throws {
        let expected: Set<String> = self == .sublab ? ["arm64"] : ["arm64", "x86_64"]
        try require(Set(value.split(whereSeparator: \.isWhitespace).map(String.init)) == expected,
                    "Das Paket enthält nicht die erwarteten Prozessor-Architekturen")
    }
    func validateMachO(_ header: Data) throws {
        // Parse the fixed Mach-O headers directly: end users need no Xcode/lipo installation.
        func word(_ offset: Int, littleEndian: Bool = false) throws -> UInt32 {
            try require(offset >= 0 && offset + 4 <= header.count, "Unvollständiger Mach-O-Header")
            let bytes = Array(header[offset..<offset + 4])
            return (littleEndian ? bytes.reversed() : bytes).reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        }
        let magic = try word(0)
        var cpus: [UInt32] = []
        switch magic {
        case 0xcffaedfe: cpus = [try word(4, littleEndian: true)]
        case 0xfeedfacf: cpus = [try word(4)]
        case 0xcafebabe, 0xcafebabf, 0xbebafeca, 0xbfbafeca:
            let little = magic == 0xbebafeca || magic == 0xbfbafeca
            let count = try word(4, littleEndian: little)
            try require(count > 0 && count <= 16, "Ungültige Anzahl Mach-O-Architekturen")
            let stride = magic == 0xcafebabf || magic == 0xbfbafeca ? 32 : 20
            for index in 0..<Int(count) { cpus.append(try word(8 + index * stride, littleEndian: little)) }
        default: throw UpdateFailure("Plugin ist keine unterstützte macOS-Binärdatei")
        }
        try require(Set(cpus).count == cpus.count, "Doppelte Mach-O-Architektur")
        let architectures = try cpus.map { cpu -> String in
            switch cpu {
            case 0x0100000c: return "arm64"
            case 0x01000007: return "x86_64"
            default: throw UpdateFailure("Unbekannte Plugin-Architektur")
            }
        }
        try validateArchitectures(architectures.joined(separator: " "))
    }
    var apiURL: URL { URL(string: "https://api.github.com/repos/TheWhykiki/\(rawValue)/releases/latest")! }
    func packageName(_ version: Version) -> String { "\(rawValue)-\(version)-macOS-\(architecture).pkg" }
    func bundle(at directory: URL) -> URL { directory.appendingPathComponent(rawValue + ".vst3", isDirectory: true) }
    var systemBundle: URL { bundle(at: URL(fileURLWithPath: "/Library/Audio/Plug-Ins/VST3", isDirectory: true)) }
    var userBundle: URL { bundle(at: FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Audio/Plug-Ins/VST3", isDirectory: true)) }
}

struct Release: Decodable {
    struct Asset: Decodable {
        let name: String
        let size: Int
        let digest: String?
        let state: String
        let browser_download_url: String
    }
    let tag_name: String
    let draft: Bool
    let prerelease: Bool
    let assets: [Asset]
}

struct UpdateCandidate {
    let version: Version
    let url: URL
    let sha256: String
    let size: Int
    static let maximumSize = 128 * 1024 * 1024

    static func select(_ release: Release, product: Product, current: Version) throws -> UpdateCandidate? {
        try require(!release.draft && !release.prerelease, "Dieses Release ist keine stabile Veröffentlichung")
        let version = try Version(release.tag_name)
        guard version > current else { return nil }
        let expected = product.packageName(version)
        let matches = release.assets.filter { $0.name == expected }
        try require(matches.count == 1, "Für Version \(version) fehlt das eindeutige Installationspaket \(expected)")
        let asset = matches[0]
        try require(asset.state == "uploaded" && asset.size > 0 && asset.size <= maximumSize, "Ungültiges oder zu großes Installationspaket")
        let prefix = "sha256:"
        guard let digest = asset.digest, digest.hasPrefix(prefix) else { throw UpdateFailure("GitHub liefert keine SHA-256-Prüfsumme für dieses Paket") }
        let hash = String(digest.dropFirst(prefix.count)).lowercased()
        try require(hash.count == 64 && hash.allSatisfy { $0.isHexDigit && $0.isASCII }, "Ungültige Paket-Prüfsumme")
        let expectedURL = "https://github.com/TheWhykiki/\(product.rawValue)/releases/download/\(release.tag_name)/\(expected)"
        try require(asset.browser_download_url == expectedURL, "Das Paket verweist nicht auf das erwartete GitHub-Release")
        return UpdateCandidate(version: version, url: URL(string: expectedURL)!, sha256: hash, size: asset.size)
    }

    func verifyDownload(_ file: URL) throws {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        var hasher = SHA256(), count = 0
        while let data = try handle.read(upToCount: 1024 * 1024), !data.isEmpty {
            count += data.count
            try require(count <= size && count <= Self.maximumSize, "Der Download ist größer als das angekündigte Paket")
            hasher.update(data: data)
        }
        try require(count == size, "Der Download ist unvollständig")
        let hash = hasher.finalize().map { String(format: "%02x", $0) }.joined()
        try require(hash == sha256, "Die Prüfsumme des Downloads stimmt nicht. Das Paket wird nicht installiert.")
    }
}

struct PackageMetadata {
    static func validate(_ data: Data, product: Product, version: Version) throws {
        let document = try XMLDocument(data: data, options: [.nodeLoadExternalEntitiesNever])
        try require(document.dtd == nil, "Paket-Metadaten dürfen keine DTD enthalten")
        guard let root = document.rootElement() else { throw UpdateFailure("Leere Paket-Metadaten") }
        try require(root.name == "pkg-info", "Nur einzelne Component-Pakete werden unterstützt")
        try require(root.attribute(forName: "identifier")?.stringValue == product.packageID,
                    "Das Paket gehört zu einem anderen Plugin")
        try require(root.attribute(forName: "version")?.stringValue == version.description,
                    "Paket-Version und Release-Version stimmen nicht überein")
        try require(root.attribute(forName: "install-location")?.stringValue == "/", "Unerwartetes Installationsziel")
        try require(root.attribute(forName: "postinstall-action")?.stringValue == "none", "Das Paket darf keinen Neustart oder Logout verlangen")
        try require(root.attribute(forName: "relocatable")?.stringValue == "false"
                    && (try root.nodes(forXPath: "relocate/*")).isEmpty, "Das Paket darf keine Installation an andere Orte umleiten")
        let bundles = try root.nodes(forXPath: "bundle")
        guard bundles.count == 1, let bundle = bundles.first as? XMLElement else { throw UpdateFailure("Unerwartete Paket-Bundles") }
        try require(bundle.attribute(forName: "id")?.stringValue == product.bundleID
                    && bundle.attribute(forName: "path")?.stringValue == "./Library/Audio/Plug-Ins/VST3/" + product.rawValue + ".vst3",
                    "Unerwarteter Plugin-Pfad im Paket")
        try require((try root.nodes(forXPath: ".//scripts")).isEmpty, "Update-Pakete dürfen keine Installationsskripte enthalten")
    }
}

func validateBundleMetadata(_ bundle: URL, product: Product, version: Version? = nil) throws -> Version {
    let data = try Data(contentsOf: bundle.appendingPathComponent("Contents/Info.plist"))
    guard let info = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else {
        throw UpdateFailure("Ungültige Plugin-Metadaten")
    }
    try require(info["CFBundleIdentifier"] as? String == product.bundleID
                && info["CFBundleExecutable"] as? String == product.rawValue, "Falsche Plugin-Identität")
    let actual = try Version(info["CFBundleShortVersionString"] as? String ?? "")
    if let version = version { try require(actual == version, "Installierte Plugin-Version stimmt nicht mit dem Update überein") }
    return actual
}
