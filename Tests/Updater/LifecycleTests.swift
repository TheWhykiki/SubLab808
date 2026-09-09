import AppKit
import Foundation
import CryptoKit

@main struct LifecycleTests {
    static var count = 0
    static func test(_ name: String, _ body: () throws -> Void) throws {
        try body(); count += 1; print("PASS " + name)
    }
    static func rejected(_ body: () throws -> Void) throws {
        do { try body() } catch { return }
        throw UpdateFailure("Expected rejection")
    }
    static func drain(_ ready: () -> Bool) throws {
        let deadline = Date().addingTimeInterval(8)
        while !ready() && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
        try require(ready(), "Asynchronous controller operation did not finish")
    }
    static func fixture(_ root: URL, pending: Bool = true) throws -> UpdaterApp {
        let app = UpdaterApp()
        app.product = .reverse
        app.current = try Version("1.1.0")
        let storeRoot = root.appendingPathComponent(UUID().uuidString)
        app.store = InstallationStore(root: storeRoot, product: .reverse)
        app.workspace = storeRoot.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: app.workspace, withIntermediateDirectories: true)
        let data = Data("isolated test package".utf8)
        let version = try Version("1.2.0")
        let file = app.workspace.appendingPathComponent(Product.reverse.packageName(version))
        try data.write(to: file)
        let url = URL(string: "https://github.com/TheWhykiki/ReverseLab/releases/download/v1.2.0/ReverseLab-1.2.0-macOS-universal.pkg")!
        let candidate = UpdateCandidate(version: version, url: url, sha256: SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined(), size: data.count)
        let prepared = PreparedPackage(file: file, candidate: candidate, fingerprint: ["test": "test"])
        app.candidate = candidate
        app.prepared = prepared
        if pending {
            app.record = InstallationRecord(product: .reverse, workspace: app.workspace, package: prepared, userCopy: nil)
            try app.store.save(app.record!)
        }
        app.operations.prepare = { file, candidate, _, _ in
            try candidate.verifyDownload(file)
            return prepared
        }
        app.operations.installed = { _, _ in false }
        app.operations.usersOfPlugin = { _ in [] }
        app.operations.rejectDowngrade = { _, _ in }
        app.operations.finishUserMigration = { _, _, _ in nil }
        app.operations.installerRunning = { false }
        app.operations.activateInstaller = {}
        app.operations.saveRecoveryHelper = { _, _ in }
        app.operations.openInstaller = { _, _ in fatalError("Unexpected Installer launch in test") }
        app.operations.confirmDeferredCompletion = { _ in fatalError("Unexpected modal prompt in test") }
        app.verificationDeadline = Date().addingTimeInterval(900)
        return app
    }

    static func main() throws {
        let application = NSApplication.shared
        application.setActivationPolicy(.prohibited)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("updater-lifecycle-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try test("closing the last window keeps pending verification alive") {
            let app = try fixture(root)
            try require(!app.applicationShouldTerminateAfterLastWindowClosed(application), "Closing the window abandons verification")
            app.record = nil
            try require(app.applicationShouldTerminateAfterLastWindowClosed(application), "Idle updater does not exit")
        }
        try test("explicit defer persists package and resumes from a new controller") {
            let app = try fixture(root)
            app.operations.confirmDeferredCompletion = { _ in true }
            try require(app.applicationShouldTerminate(application) == .terminateNow, "Cannot defer unfinished installation")
            app.applicationWillTerminate(Notification(name: NSApplication.willTerminateNotification))
            try require(FileManager.default.fileExists(atPath: app.prepared!.file.path), "Pending package deleted")
            let next = try fixture(root)
            next.store = app.store
            next.record = try next.store.load()
            next.workspace = try next.store.workspace(for: next.record!)
            next.candidate = try next.record!.candidate(for: .reverse)
            next.prepared = nil
            let expected = app.prepared!
            var revalidated = false
            next.operations.prepare = { file, candidate, _, _ in
                try candidate.verifyDownload(file); revalidated = true; return expected
            }
            next.resumeInstallation()
            try drain { revalidated && !next.busy && !next.checkingInstallation && next.step == .retry }
            try require(next.prepared?.candidate.version == expected.candidate.version, "Resume changed target version")
        }
        try test("handoff and migration cannot be interrupted by normal quit") {
            let app = try fixture(root)
            app.checkingInstallation = true
            try require(app.applicationShouldTerminate(application) == .terminateCancel, "Quit during migration")
            app.checkingInstallation = false; app.busy = true; app.step = .install
            try require(app.applicationShouldTerminate(application) == .terminateCancel, "Quit during Installer handoff")
        }
        try test("canceled Installer exposes retry and stops polling") {
            let app = try fixture(root)
            app.startVerification()
            try drain { !app.checkingInstallation }
            try require(app.step == .retry && app.button.title == "Installation fortsetzen", "No retry action")
            try require(app.timer?.isValid != true && app.record != nil, "Canceled installation not safely paused")
        }
        try test("retry checks installed files first and opens exactly once") {
            let app = try fixture(root)
            var checks = 0, launches = 0
            app.operations.installed = { _, _ in checks += 1; return false }
            app.operations.openInstaller = { _, completion in
                launches += 1
                do { try require(try app.store.load()?.id == app.record?.id, "Journal missing at handoff") }
                catch { completion(error); return }
                app.operations.installerRunning = { true }
                completion(nil)
            }
            app.step = .retry
            app.primaryAction()
            try drain { launches == 1 && !app.busy && !app.checkingInstallation }
            app.timer?.invalidate()
            try require(checks >= 1 && launches == 1, "Retry skipped verification or duplicated Installer")
            app.beginInstallation()
            try require(launches == 1, "Opened a second active Installer")
        }
        try test("first handoff is journaled before Installer is opened") {
            let app = try fixture(root, pending: false)
            var durable = false
            app.operations.openInstaller = { _, completion in
                durable = (try? app.store.load()) != nil
                completion(UpdateFailure("Simulated launch failure"))
            }
            app.beginInstallation()
            try drain { !app.busy }
            try require(durable && app.record != nil && app.step == .retry, "Failed handoff cannot resume")
        }
        try test("modified package cannot reach Installer on retry") {
            let app = try fixture(root)
            try Data("corrupt".utf8).write(to: app.prepared!.file)
            app.beginInstallation()
            try drain { !app.busy }
            try require(app.step == .download && app.record != nil && app.prepared == nil && app.button.title == "Paket erneut laden", "Corrupt package has no safe re-download action")
        }
        try test("active DAW use prevents Installer handoff") {
            let app = try fixture(root)
            app.operations.usersOfPlugin = { _ in [123] }
            app.beginInstallation()
            try drain { !app.busy }
            try require(app.detail.stringValue.contains("DAWs"), "Active host was not blocked")
        }
        try test("polling pauses at deadline without claiming cancellation or success") {
            let app = try fixture(root)
            app.operations.installerRunning = { true }
            app.operations.now = { Date.distantFuture }
            app.verifyInstallation()
            try drain { !app.checkingInstallation }
            try require(app.step == .retry && app.status.stringValue == "Installation noch offen", "Deadline state incorrect")
        }
        try test("success finalizes migration once and clears the durable record") {
            let app = try fixture(root)
            var migrations = 0
            app.operations.installed = { _, _ in true }
            app.operations.finishUserMigration = { _, _, _ in migrations += 1; return nil }
            app.verifyInstallation()
            try drain { !app.checkingInstallation }
            try require(app.step == .finished && app.record == nil && migrations == 1, "Verified success incomplete")
            try require(try app.store.load() == nil, "Completed journal not cleared")
            app.verifyInstallation()
            try require(migrations == 1, "Migration repeated after success")
        }
        try test("failed migration preserves recovery information") {
            let app = try fixture(root)
            app.operations.installed = { _, _ in true }
            app.operations.finishUserMigration = { _, _, _ in throw UpdateFailure("Changed user copy") }
            app.verifyInstallation()
            try drain { !app.checkingInstallation }
            try require(app.step == .retry && app.record != nil && (try app.store.load()) != nil, "Migration failure lost journal")
        }
        try test("mismatched installed payload offers explicit repair after Installer exits") {
            let app = try fixture(root)
            app.operations.installed = { _, _ in throw UpdateFailure("Payload mismatch") }
            app.verifyInstallation()
            try drain { !app.checkingInstallation }
            try require(app.repairNeeded && app.button.title == "Installation reparieren", "Damaged installation cannot be repaired")
            var launches = 0
            app.operations.openInstaller = { _, completion in launches += 1; completion(UpdateFailure("Test launch")) }
            app.primaryAction()
            try drain { !app.busy }
            try require(launches == 1 && app.record != nil, "Explicit repair did not reopen the verified package")
        }
        try test("intermediate Installer payload does not trigger repair or success") {
            let app = try fixture(root)
            app.operations.installerRunning = { true }
            app.operations.installed = { _, _ in throw UpdateFailure("Intermediate files") }
            app.verifyInstallation()
            try drain { !app.checkingInstallation }
            try require(!app.repairNeeded && app.step == .verify, "Repair offered during active installation")
        }
        try test("missing pending package offers a verified re-download") {
            let app = try fixture(root)
            try FileManager.default.removeItem(at: app.prepared!.file)
            app.prepared = nil
            app.resumeInstallation()
            try drain { !app.busy }
            try require(app.step == .resume && app.resumeFailed && app.button.title == "Paket erneut laden", "Missing download blocks recovery")
        }
        try test("journal validates product, path, URL, schema and snapshot consistency") {
            let app = try fixture(root)
            let data = try JSONEncoder().encode(app.record!)
            let original = try JSONSerialization.jsonObject(with: data) as! [String: Any]
            for (key, value) in [("productName", "SubLab808" as Any), ("id", "../../escape"),
                                 ("downloadURL", "https://evil.example/update.pkg"), ("schema", 2), ("userVersion", "1.1.0")] {
                var invalid = original; invalid[key] = value
                try JSONSerialization.data(withJSONObject: invalid).write(to: app.store.root.appendingPathComponent("pending.json"))
                try rejected { _ = try app.store.load() }
            }
        }
        try test("journal refuses a symlinked transaction directory") {
            let app = try fixture(root)
            try FileManager.default.removeItem(at: app.workspace)
            try FileManager.default.createSymbolicLink(at: app.workspace, withDestinationURL: root)
            try rejected { _ = try app.store.load() }
        }
        print("\(count) lifecycle tests passed against the production AppKit controller; no windows, Installer or installed plugins were used.")
    }
}
