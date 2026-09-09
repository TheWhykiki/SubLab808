import Foundation

// Written before handing a package to Installer. Only product-relative locations
// are persisted: a damaged record can never select an arbitrary plugin or backup.
struct InstallationRecord: Codable, Equatable {
    let schema: Int
    let productName: String
    let id: String
    let version: String
    let downloadURL: String
    let sha256: String
    let size: Int
    let userVersion: String?
    let userFingerprint: [String: String]?

    init(product: Product, workspace: URL, package: PreparedPackage, userCopy: UserCopy?) {
        schema = 1
        productName = product.rawValue
        id = workspace.lastPathComponent
        version = package.candidate.version.description
        downloadURL = package.candidate.url.absoluteString
        sha256 = package.candidate.sha256
        size = package.candidate.size
        userVersion = userCopy?.version.description
        userFingerprint = userCopy?.fingerprint
    }

    func candidate(for product: Product) throws -> UpdateCandidate {
        try require(schema == 1 && productName == product.rawValue && UUID(uuidString: id)?.uuidString == id,
                    "Der gespeicherte Update-Auftrag ist ungültig")
        let parsed = try Version(version)
        let tag = URL(string: downloadURL)?.deletingLastPathComponent().lastPathComponent ?? ""
        try require(try Version(tag) == parsed, "Gespeicherte Release-Version stimmt nicht überein")
        let release = Release(tag_name: tag, draft: false, prerelease: false, assets: [
            Release.Asset(name: product.packageName(parsed), size: size, digest: "sha256:" + sha256,
                          state: "uploaded", browser_download_url: downloadURL)
        ])
        guard let candidate = try UpdateCandidate.select(release, product: product, current: Version("0.0.0")) else {
            throw UpdateFailure("Gespeicherte Update-Version ist ungültig")
        }
        if let userVersion = userVersion, let fingerprint = userFingerprint {
            try require(try Version(userVersion) < candidate.version && !fingerprint.isEmpty,
                        "Gespeicherte Benutzerinstallation ist ungültig")
        } else {
            try require(userVersion == nil && userFingerprint == nil, "Unvollständige Sicherungsdaten")
        }
        return candidate
    }

    func userCopy(for product: Product) throws -> UserCopy? {
        _ = try candidate(for: product)
        guard let version = userVersion, let fingerprint = userFingerprint else { return nil }
        return UserCopy(url: product.userBundle, version: try Version(version), fingerprint: fingerprint)
    }
}

struct InstallationStore {
    let root: URL
    let product: Product
    private var file: URL { root.appendingPathComponent("pending.json") }

    func workspace(for record: InstallationRecord) throws -> URL {
        _ = try record.candidate(for: product)
        let url = root.appendingPathComponent(record.id, isDirectory: true)
        try rejectSymlinkAncestors(url)
        return url
    }

    func load() throws -> InstallationRecord? {
        try rejectSymlinkAncestors(file)
        guard FileManager.default.fileExists(atPath: file.path) else { return nil }
        let size = try file.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
        try require(size > 0 && size <= 4 * 1024 * 1024, "Gespeicherter Update-Auftrag ist zu groß oder leer")
        let record = try JSONDecoder().decode(InstallationRecord.self, from: Data(contentsOf: file))
        _ = try workspace(for: record)
        return record
    }

    func save(_ record: InstallationRecord) throws {
        _ = try workspace(for: record)
        try rejectSymlinkAncestors(file)
        let data = try JSONEncoder().encode(record)
        try require(data.count <= 4 * 1024 * 1024, "Update-Auftrag ist zu groß")
        try data.write(to: file, options: .atomic)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
        // Flush the handoff record before Installer can change the system bundle.
        let handle = try FileHandle(forWritingTo: file)
        defer { try? handle.close() }
        try handle.synchronize()
    }

    func complete(_ record: InstallationRecord) throws {
        try require(try load() == record, "Der gespeicherte Update-Auftrag wurde zwischenzeitlich geändert")
        try FileManager.default.removeItem(at: file)
    }
}
