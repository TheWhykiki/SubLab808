import Foundation
import CryptoKit

@main struct UpdaterTests {
    static var count = 0
    static func test(_ name: String, _ body: () throws -> Void) throws {
        try body()
        count += 1
        print("PASS " + name)
    }
    static func rejected(_ body: () throws -> Void) throws {
        do { try body() } catch { return }
        throw UpdateFailure("Expected rejection")
    }
    static func release(_ product: Product = .reverse, version: String = "1.2.0", url: String? = nil,
                        digest: String? = "sha256:" + String(repeating: "a", count: 64), size: Int = 32,
                        draft: Bool = false, prerelease: Bool = false) throws -> Release {
        let parsed = try Version(version)
        let name = product.packageName(parsed)
        return Release(tag_name: "v" + version, draft: draft, prerelease: prerelease,
                       assets: [Release.Asset(name: name, size: size, digest: digest, state: "uploaded",
                                              browser_download_url: url ?? "https://github.com/TheWhykiki/\(product.rawValue)/releases/download/v\(version)/\(name)")])
    }

    static func main() throws {
        let temporary = FileManager.default.temporaryDirectory.resolvingSymlinksInPath().appendingPathComponent("whykiki-updater-tests-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: temporary) }
        let current = try Version("1.1.0")
        try test("numeric version ordering") { try require(try Version("1.10.0") > Version("1.9.0"), "Versions compared as text") }
        try test("invalid and prerelease versions") {
            for value in ["1.2", "01.2.3", "1.2.3-beta", "1.2.-3", "v1.2.3+build", "١.2.3", "1.2.9999999"] {
                try rejected { _ = try Version(value) }
            }
        }
        try test("both products select only their package") {
            for product in Product.allCases {
                let candidate = try UpdateCandidate.select(release(product), product: product, current: current)
                try require(candidate?.version == Version("1.2.0"), "Update not selected")
            }
        }
        try test("equal versions and downgrades never install") {
            for version in ["1.0.0", "1.1.0"] {
                try require(try UpdateCandidate.select(release(version: version), product: .reverse, current: current) == nil, "Downgrade selected")
            }
        }
        try test("draft and prerelease rejection") {
            try rejected { _ = try UpdateCandidate.select(release(draft: true), product: .reverse, current: current) }
            try rejected { _ = try UpdateCandidate.select(release(prerelease: true), product: .reverse, current: current) }
        }
        try test("missing digest rejection") {
            try rejected { _ = try UpdateCandidate.select(release(digest: nil), product: .reverse, current: current) }
            try rejected { _ = try UpdateCandidate.select(release(digest: "sha256:bad"), product: .reverse, current: current) }
        }
        try test("wrong repository and transport rejection") {
            for url in ["http://github.com/TheWhykiki/ReverseLab/update.pkg", "https://evil.example/update.pkg", "https://github.com/other/repo/update.pkg"] {
                try rejected { _ = try UpdateCandidate.select(release(url: url), product: .reverse, current: current) }
            }
        }
        try test("wrong product rejection") {
            try rejected { _ = try UpdateCandidate.select(release(.sublab), product: .reverse, current: current) }
        }
        try test("product architecture requirements") {
            try Product.sublab.validateArchitectures("arm64\n")
            try Product.reverse.validateArchitectures("x86_64 arm64\n")
            try rejected { try Product.sublab.validateArchitectures("x86_64") }
            try rejected { try Product.reverse.validateArchitectures("arm64") }
        }
        try test("Mach-O architecture inspection without developer tools") {
            let arm = Data([0xcf, 0xfa, 0xed, 0xfe, 0x0c, 0, 0, 1])
            try Product.sublab.validateMachO(arm)
            try rejected { try Product.reverse.validateMachO(arm) }
            var fat = Data([0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 2, 1, 0, 0, 12])
            fat.append(Data(repeating: 0, count: 16))
            fat.append(contentsOf: [1, 0, 0, 7])
            try Product.reverse.validateMachO(fat)
            try rejected { try Product.sublab.validateMachO(fat) }
            try rejected { try Product.reverse.validateMachO(fat.dropLast()) }
            try rejected { try Product.sublab.validateMachO(Data([0, 1, 2])) }
        }
        try test("ambiguous package rejection") {
            let base = try release()
            let duplicate = Release(tag_name: base.tag_name, draft: false, prerelease: false, assets: base.assets + base.assets)
            try rejected { _ = try UpdateCandidate.select(duplicate, product: .reverse, current: current) }
        }
        try test("empty and oversized release rejection") {
            for size in [0, UpdateCandidate.maximumSize + 1] {
                try rejected { _ = try UpdateCandidate.select(release(size: size), product: .reverse, current: current) }
            }
        }
        let file = temporary.appendingPathComponent("download.pkg")
        let content = Data("verified payload".utf8)
        let hash = SHA256.hash(data: content).map { String(format: "%02x", $0) }.joined()
        let candidate = UpdateCandidate(version: try Version("1.2.0"), url: URL(string: "https://github.com")!, sha256: hash, size: content.count)
        try test("complete download accepted") { try content.write(to: file); try candidate.verifyDownload(file) }
        try test("truncated download rejection") {
            try content.dropLast().write(to: file)
            try rejected { try candidate.verifyDownload(file) }
        }
        try test("corrupt equal-size download rejection") {
            try Data(repeating: 0, count: content.count).write(to: file)
            try rejected { try candidate.verifyDownload(file) }
        }
        try test("package metadata binds product version and path") {
            let valid = "<pkg-info identifier='audio.whykiki.reverselab.pkg' version='1.2.0' install-location='/' relocatable='false' postinstall-action='none'><bundle id='audio.whykiki.reverselab' path='./Library/Audio/Plug-Ins/VST3/ReverseLab.vst3'/></pkg-info>"
            try PackageMetadata.validate(Data(valid.utf8), product: .reverse, version: candidate.version)
            for invalid in [valid.replacingOccurrences(of: "reverselab", with: "sublab808"),
                            valid.replacingOccurrences(of: "1.2.0", with: "1.1.0"),
                            valid.replacingOccurrences(of: "install-location='/'", with: "install-location='/tmp'"),
                            valid.replacingOccurrences(of: "relocatable='false'", with: "relocatable='true'"),
                            valid.replacingOccurrences(of: "postinstall-action='none'", with: "postinstall-action='restart'"),
                            valid.replacingOccurrences(of: "</pkg-info>", with: "<relocate><bundle/></relocate></pkg-info>"),
                            valid.replacingOccurrences(of: "</pkg-info>", with: "<scripts/></pkg-info>"),
                            "<!DOCTYPE pkg-info [<!ENTITY unused SYSTEM 'file:///etc/hosts'>]>" + valid] {
                try rejected { try PackageMetadata.validate(Data(invalid.utf8), product: .reverse, version: candidate.version) }
            }
        }
        let bundle = temporary.appendingPathComponent("ReverseLab.vst3", isDirectory: true)
        try FileManager.default.createDirectory(at: bundle.appendingPathComponent("Contents"), withIntermediateDirectories: true)
        let plist: [String: Any] = ["CFBundleIdentifier": "audio.whykiki.reverselab", "CFBundleExecutable": "ReverseLab", "CFBundleShortVersionString": "1.1.0"]
        try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0).write(to: bundle.appendingPathComponent("Contents/Info.plist"))
        try test("bundle metadata validation") {
            _ = try validateBundleMetadata(bundle, product: .reverse, version: current)
            try rejected { _ = try validateBundleMetadata(bundle, product: .sublab) }
        }
        let copy = UserCopy(url: bundle, version: current, fingerprint: try bundleFingerprint(bundle))
        try test("changed user copy never moved") {
            let extra = bundle.appendingPathComponent("changed")
            try Data("newer change".utf8).write(to: extra)
            try rejected { _ = try copy.archive(to: temporary.appendingPathComponent("rejected-backup")) }
            try require(FileManager.default.fileExists(atPath: extra.path), "Changed bundle was moved")
            try FileManager.default.removeItem(at: extra)
        }
        try test("user copy archived without merging or deletion") {
            let backup = try copy.archive(to: temporary.appendingPathComponent("backup"))
            try require(!FileManager.default.fileExists(atPath: bundle.path), "Old bundle still shadows system install")
            try require(try bundleFingerprint(backup) == copy.fingerprint, "Backup changed")
        }
        try test("symlink target rejected") {
            let link = temporary.appendingPathComponent("redirect")
            try FileManager.default.createSymbolicLink(at: link, withDestinationURL: temporary.appendingPathComponent("backup"))
            try rejected { try rejectSymlinkAncestors(link.appendingPathComponent("other")) }
        }
        try test("unsigned macOS package rejected before installation") {
            let package = temporary.appendingPathComponent("unsigned.pkg")
            try verifiedTool("/usr/bin/pkgbuild", ["--root", temporary.appendingPathComponent("backup").path,
                "--identifier", "audio.whykiki.reverselab.pkg", "--version", "1.2.0", package.path], message: "Test package could not be built")
            let bytes = try Data(contentsOf: package)
            let unsigned = UpdateCandidate(version: candidate.version, url: candidate.url,
                sha256: SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined(), size: bytes.count)
            do {
                _ = try PackageService.prepare(package, candidate: unsigned, product: .reverse, workspace: temporary)
                throw UpdateFailure("Unsigned package unexpectedly accepted")
            } catch let error as UpdateFailure {
                try require(error.message.contains("Installer-Signatur"), "Unsigned package failed at the wrong gate: " + error.message)
            }
        }
        print("\(count) updater tests passed; no network, installed plugin or Installer was used.")
    }
}
