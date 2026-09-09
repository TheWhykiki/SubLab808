import AppKit
import Foundation
import Darwin

final class UpdaterApp: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var window: NSWindow!
    let status = NSTextField(wrappingLabelWithString: "")
    let detail = NSTextField(wrappingLabelWithString: "")
    let button = NSButton(title: "Nach Updates suchen", target: nil, action: nil)
    let closeButton = NSButton(title: "Schließen", target: nil, action: nil)
    let progress = NSProgressIndicator()
    var product: Product!
    var current: Version!
    var host: NSRunningApplication?
    var workspace: URL!
    var userCopy: UserCopy?
    var candidate: UpdateCandidate?
    var prepared: PreparedPackage?
    var client: HTTPClient?
    var generation = 0
    var timer: Timer?
    var busy = false
    var record: InstallationRecord?
    var store: InstallationStore!
    var operations = UpdaterOperations()
    var verificationDeadline = Date.distantPast
    var resumeFailed = false
    var repairNeeded = false
    var checkingInstallation = false
    var lockDescriptor: Int32 = -1
    enum Step { case check, download, install, verify, retry, resume, finished }
    var step = Step.check

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildWindow()
        do {
            guard let name = Bundle.main.object(forInfoDictionaryKey: "WKProduct") as? String,
                  let selected = Product(rawValue: name) else { throw UpdateFailure("Updater-Produktkonfiguration fehlt") }
            product = selected
            let cache = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
                .resolvingSymlinksInPath().appendingPathComponent("Whykiki Audio/Updates/" + product.rawValue, isDirectory: true)
            try rejectSymlinkAncestors(cache)
            try FileManager.default.createDirectory(at: cache, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            lockDescriptor = open(cache.appendingPathComponent("update.lock").path, O_RDWR | O_CREAT | O_NOFOLLOW, 0o600)
            try require(lockDescriptor >= 0 && flock(lockDescriptor, LOCK_EX | LOCK_NB) == 0, "Für dieses Plugin ist bereits ein Updater geöffnet.")
            store = InstallationStore(root: cache, product: product)
            let pending = try store.load()
            let arguments = CommandLine.arguments
            if arguments.count == 1 && pending != nil {
                // The recovery app is usable even if an interrupted system install
                // temporarily prevents the DAW from loading the plugin.
                current = try Version(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "")
            } else {
                try require(arguments.count == 5 && arguments[1] == "--plugin", "Bitte den Updater über die Updates-Schaltfläche im Plugin starten.")
                let plugin = URL(fileURLWithPath: arguments[2]).standardizedFileURL
                try require(plugin == product.userBundle.standardizedFileURL || plugin == product.systemBundle.standardizedFileURL,
                            "Automatische Updates sind für Installationen in /Library/Audio/Plug-Ins/VST3 oder ~/Library/Audio/Plug-Ins/VST3 verfügbar.")
                try rejectSymlinkAncestors(plugin)
                current = try Version(arguments[3])
                _ = try validateBundleMetadata(plugin, product: product, version: current)
                guard let pid = Int32(arguments[4]), pid > 1, let running = NSRunningApplication(processIdentifier: pid) else {
                    throw UpdateFailure("Die aufrufende DAW konnte nicht identifiziert werden")
                }
                host = running
            }
            window.title = product.rawValue + " Update"
            detail.stringValue = "Installiert: \(current!) · Installationsziel: /Library/Audio/Plug-Ins/VST3\nDer Updater bleibt beim Schließen der DAW geöffnet."
            if let pending = pending {
                record = pending
                candidate = try pending.candidate(for: product)
                userCopy = try pending.userCopy(for: product)
                workspace = try store.workspace(for: pending)
                resumeInstallation()
            } else {
                userCopy = try UserCopy.capture(product)
                workspace = cache.appendingPathComponent(UUID().uuidString, isDirectory: true)
                try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
                checkRelease()
            }
        } catch { fail(error, retry: false) }
    }

    func buildWindow() {
        let menu = NSMenu()
        let applicationItem = NSMenuItem()
        let applicationMenu = NSMenu()
        applicationMenu.addItem(withTitle: "Updater beenden", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        applicationItem.submenu = applicationMenu
        menu.addItem(applicationItem)
        NSApp.mainMenu = menu
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 600, height: 360),
                          styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "Whykiki Audio Update"
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.center()
        status.font = .systemFont(ofSize: 20, weight: .semibold)
        status.maximumNumberOfLines = 2
        detail.font = .systemFont(ofSize: 13)
        detail.maximumNumberOfLines = 7
        progress.style = .bar
        progress.isIndeterminate = false
        progress.minValue = 0
        progress.maxValue = 1
        button.bezelStyle = .rounded
        button.target = self
        button.action = #selector(primaryAction)
        closeButton.bezelStyle = .rounded
        closeButton.target = self
        closeButton.action = #selector(closeWindow)
        let buttons = NSStackView(views: [closeButton, button])
        buttons.orientation = .horizontal
        buttons.alignment = .centerY
        buttons.spacing = 12
        let stack = NSStackView(views: [status, detail, progress, buttons])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 18
        stack.translatesAutoresizingMaskIntoConstraints = false
        window.contentView!.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor, constant: 28),
            stack.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor, constant: -28),
            stack.topAnchor.constraint(equalTo: window.contentView!.topAnchor, constant: 28),
            progress.widthAnchor.constraint(equalTo: stack.widthAnchor),
            detail.widthAnchor.constraint(equalTo: stack.widthAnchor),
            status.widthAnchor.constraint(equalTo: stack.widthAnchor)
        ])
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    var criticalOperation: Bool { checkingInstallation || (busy && step == .install) }
    @objc func closeWindow() {
        if record != nil { window?.performClose(nil) }
        else { NSApp.terminate(nil) }
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { !criticalOperation }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if criticalOperation { return .terminateCancel }
        if record != nil {
            let deferUpdate = operations.confirmDeferredCompletion(workspace.appendingPathComponent(product.rawValue + "Updater.app"))
            return deferUpdate && !criticalOperation ? .terminateNow : .terminateCancel
        }
        return .terminateNow
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { record == nil }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        window?.makeKeyAndOrderFront(nil)
        return true
    }
    func applicationWillTerminate(_ notification: Notification) {
        generation += 1
        client?.cancel()
        timer?.invalidate()
        // A pending package belongs to a durable transaction, not a disposable download.
        if record == nil, !operations.installerRunning(), let workspace = workspace {
            try? FileManager.default.removeItem(at: workspace)
        }
        if lockDescriptor >= 0 { flock(lockDescriptor, LOCK_UN); close(lockDescriptor) }
    }

    func fail(_ error: Error, retry: Bool = true) {
        busy = false
        status.stringValue = "Update nicht abgeschlossen"
        detail.stringValue = error.localizedDescription
        button.isEnabled = retry
        timer?.invalidate()
        if record != nil {
            window?.makeKeyAndOrderFront(nil)
            step = prepared == nil ? .resume : .retry
            button.title = prepared == nil ? "Wiederaufnahme versuchen" : "Installation fortsetzen"
        } else {
            step = .check
            button.title = "Erneut versuchen"
        }
    }

    @objc func primaryAction() {
        guard !busy else { return }
        switch step {
        case .check: checkRelease()
        case .download: download()
        case .install: beginInstallation()
        case .verify:
            operations.activateInstaller()
            verifyInstallation()
        case .retry:
            if repairNeeded { beginInstallation() } else { verifyInstallation(retryIfMissing: true) }
        case .resume:
            if resumeFailed { download() } else { resumeInstallation() }
        case .finished: closeWindow()
        }
    }

    func checkRelease() {
        guard let product = product, let current = current else { return }
        busy = true
        button.isEnabled = false
        status.stringValue = "Suche nach Updates …"
        generation += 1
        let token = generation
        client = HTTPClient(url: product.apiURL, limit: 1024 * 1024, configuration: operations.httpConfiguration()) { [weak self] result in
            guard let self = self, self.generation == token else { return }
            self.busy = false
            do {
                let release = try JSONDecoder().decode(Release.self, from: result.get())
                guard let candidate = try UpdateCandidate.select(release, product: product, current: current) else {
                    let latest = try Version(release.tag_name)
                    self.status.stringValue = current > latest ? "Installierte Version ist neuer" : "Aktuelle Version installiert"
                    self.detail.stringValue = "Installiert: \(current) · Aktuelles GitHub-Release: \(latest)\n" + (current > latest ? "Dein Build ist neuer als die veröffentlichte Version. Ein Downgrade wird nicht angeboten." : "Es gibt keine neuere stabile Version für dieses Plugin.")
                    self.step = .finished
                    self.button.title = "Fertig"
                    self.button.isEnabled = true
                    return
                }
                if let copy = self.userCopy { try require(copy.version < candidate.version, "Die Benutzerinstallation ist bereits gleich neu oder neuer. Sie wird nicht ersetzt.") }
                try self.operations.rejectDowngrade(product, candidate.version)
                self.candidate = candidate
                self.status.stringValue = "Version \(candidate.version) ist verfügbar"
                self.detail.stringValue = "Installiert: \(current) · Download: \(ByteCountFormatter.string(fromByteCount: Int64(candidate.size), countStyle: .file))\nDas Paket wird geprüft. Vor der Installation musst du dein Projekt speichern und alle DAWs schließen. macOS fragt bei Bedarf nach dem Administratorpasswort."
                self.step = .download
                self.button.title = "Update laden & installieren"
                self.button.isEnabled = true
            } catch HTTPFailure.noRelease {
                self.status.stringValue = "Noch kein GitHub-Release"
                self.detail.stringValue = "Installiert: \(current). Für \(product.rawValue) wurde noch kein stabiles Release veröffentlicht. Sobald ein Release mit Installationspaket verfügbar ist, erscheint es hier."
                self.step = .check
                self.button.title = "Erneut prüfen"
                self.button.isEnabled = true
            } catch { self.fail(error) }
        }
    }

    func download() {
        guard let candidate = candidate else { return }
        if record != nil && operations.installerRunning() {
            detail.stringValue = "Bitte den geöffneten macOS-Installer beenden, bevor das Paket erneut geladen wird."
            operations.activateInstaller()
            return
        }
        busy = true
        button.isEnabled = false
        status.stringValue = "Update wird geladen …"
        let package = workspace.appendingPathComponent(product.packageName(candidate.version))
        do {
            try rejectSymlinkAncestors(workspace)
            try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        } catch { fail(error); return }
        try? FileManager.default.removeItem(at: package)
        let token = generation
        client = HTTPClient(url: candidate.url, destination: package, limit: candidate.size, configuration: operations.httpConfiguration(),
                            progress: { [weak self] value in self?.progress.doubleValue = value }) { [weak self] result in
            guard let self = self, self.generation == token else { return }
            do { _ = try result.get() } catch { self.fail(error); return }
            self.status.stringValue = "Prüfe Installationspaket …"
            let product = self.product!, workspace = self.workspace!
            DispatchQueue.global(qos: .userInitiated).async {
                let prepared = Result { try self.operations.prepare(package, candidate, product, workspace) }
                DispatchQueue.main.async {
                    guard self.generation == token else { return }
                    self.busy = false
                    switch prepared {
                    case .failure(let error): self.fail(error)
                    case .success(let package):
                        self.prepared = package
                        self.resumeFailed = false
                        if self.record != nil {
                            self.startVerification()
                            return
                        }
                        self.status.stringValue = "Bereit zur Installation"
                        self.detail.stringValue = "Bitte Projekt speichern und alle DAWs schließen. Danach auf Installieren klicken. Eine vorhandene ältere Benutzerinstallation wird nach erfolgreicher Installation gesichert, damit sie die neue Version nicht verdeckt."
                        self.step = .install
                        self.button.title = "Installieren"
                        self.button.isEnabled = true
                    }
                }
            }
        }
    }

    func resumeInstallation() {
        guard let candidate = candidate, record != nil else { return }
        busy = true
        button.isEnabled = false
        status.stringValue = "Gespeicherten Update-Auftrag prüfen …"
        let package = workspace.appendingPathComponent(product.packageName(candidate.version))
        let product = product!, workspace = workspace!
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try self.operations.prepare(package, candidate, product, workspace) }
            DispatchQueue.main.async {
                self.busy = false
                switch result {
                case .success(let prepared):
                    self.prepared = prepared
                    self.resumeFailed = false
                    self.startVerification()
                case .failure(let error):
                    self.fail(error)
                    self.resumeFailed = true
                    self.button.title = "Paket erneut laden"
                }
            }
        }
    }

    func beginInstallation() {
        guard let prepared = prepared else { return }
        if operations.installerRunning() {
            detail.stringValue = "macOS-Installer ist bereits geöffnet. Schließe eine abgebrochene Installation und beende Installer, um es erneut zu versuchen."
            operations.activateInstaller()
            return
        }
        guard host?.isTerminated != false else {
            detail.stringValue = "Die aufrufende DAW ist noch geöffnet. Bitte Projekt speichern und die DAW vollständig beenden. Dieser Updater bleibt geöffnet."
            return
        }
        step = .install
        busy = true
        button.isEnabled = false
        let product = product!
        DispatchQueue.global(qos: .userInitiated).async {
            let ready = Result {
                try self.operations.rejectDowngrade(product, prepared.candidate.version)
                try require(try self.operations.usersOfPlugin(product).isEmpty, "Eine weitere Anwendung verwendet dieses Plugin. Bitte alle DAWs schließen.")
                do { try prepared.candidate.verifyDownload(prepared.file) }
                catch { throw DownloadIntegrityFailure(underlying: error) }
            }
            DispatchQueue.main.async {
                do {
                    try ready.get()
                    // Commit the recovery information before Installer can touch the plugin.
                    let record = self.record ?? InstallationRecord(product: product, workspace: self.workspace, package: prepared, userCopy: self.userCopy)
                    try self.operations.saveRecoveryHelper(self.workspace.appendingPathComponent(product.rawValue + "Updater.app"), product)
                    try self.store.save(record)
                    self.record = record
                    self.closeButton.title = "Fenster schließen"
                    // Recheck after the asynchronous preparation; another Installer may have opened.
                    try require(!self.operations.installerRunning(), "macOS-Installer wurde inzwischen geöffnet. Bitte zuerst diese Installation abschließen.")
                    self.operations.openInstaller(prepared.file) { error in
                        DispatchQueue.main.async {
                            self.busy = false
                            if let error = error { self.fail(error); return }
                            self.startVerification()
                        }
                    }
                } catch let error as DownloadIntegrityFailure {
                    self.prepared = nil
                    self.resumeFailed = true
                    self.fail(error)
                    // Before the first handoff a fresh download also suffices.
                    self.step = .download
                    self.button.title = "Paket erneut laden"
                } catch {
                    self.busy = false
                    self.button.isEnabled = true
                    self.detail.stringValue = error.localizedDescription
                    self.step = self.record == nil ? .install : .retry
                    self.button.title = self.record == nil ? "Installieren" : "Installation fortsetzen"
                }
            }
        }
    }

    func startVerification() {
        repairNeeded = false
        step = .verify
        closeButton.title = "Fenster schließen"
        status.stringValue = "Installation in macOS abschließen"
        detail.stringValue = "Folge dem macOS-Installer und halte die DAWs geschlossen. Beim Schließen dieses Fensters läuft die Prüfung weiter. Über das Dock kannst du es wieder öffnen."
        button.title = "Installer anzeigen / prüfen"
        button.isEnabled = true
        verificationDeadline = operations.now().addingTimeInterval(15 * 60)
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.verifyInstallation() }
        verifyInstallation()
    }

    func verifyInstallation(retryIfMissing: Bool = false) {
        guard let prepared = prepared, let record = record, !checkingInstallation, !busy else { return }
        checkingInstallation = true
        button.isEnabled = false
        let product = product!, copy = userCopy
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { () -> String? in
                do { guard try self.operations.installed(prepared, product) else { return nil } }
                catch { throw InstalledPayloadFailure(underlying: error) }
                try require(self.host?.isTerminated != false, "Bitte die aufrufende DAW schließen, bevor das Update abgeschlossen wird.")
                try require(try self.operations.usersOfPlugin(product).isEmpty, "Bitte die DAWs schließen, bevor die Installation abgeschlossen wird.")
                let backup = try self.operations.finishUserMigration(product, copy, record.id)
                try self.store.complete(record)
                return "Version \(prepared.candidate.version) wurde geprüft und installiert. Starte die DAW neu; falls nötig, führe einen Plugin-Scan aus."
                    + (backup.map { "\nVorherige Benutzerinstallation: " + $0.path } ?? "")
            }
            DispatchQueue.main.async {
                self.checkingInstallation = false
                self.button.isEnabled = true
                switch result {
                case .failure(let error as InstalledPayloadFailure):
                    if self.operations.installerRunning() {
                        // Installer can expose intermediate files. Wait until it exits
                        // before offering an explicit repair of a mismatched payload.
                        self.showUnfinishedInstallation()
                    } else {
                        self.fail(error)
                        self.repairNeeded = true
                        self.button.title = "Installation reparieren"
                    }
                case .failure(let error): self.fail(error)
                case .success(nil):
                    if retryIfMissing && !self.operations.installerRunning() {
                        self.beginInstallation()
                    } else {
                        self.showUnfinishedInstallation()
                    }
                case .success(let message?):
                    self.timer?.invalidate()
                    self.record = nil
                    self.status.stringValue = "Update installiert"
                    self.detail.stringValue = message
                    self.userCopy = nil
                    self.step = .finished
                    self.button.title = "Fertig"
                    self.closeButton.title = "Schließen"
                    self.window?.makeKeyAndOrderFront(nil)
                }
            }
        }
    }

    func showUnfinishedInstallation() {
        let running = operations.installerRunning()
        let expired = operations.now() >= verificationDeadline
        if !running || expired {
            timer?.invalidate()
            step = .retry
            status.stringValue = running ? "Installation noch offen" : "Installation nicht abgeschlossen"
            detail.stringValue = running
                ? "Die automatische Prüfung pausiert nach 15 Minuten. Schließe den Vorgang im Installer ab oder beende Installer nach einem Abbruch. Mit Installation fortsetzen wird zuerst geprüft und bei Bedarf der Installer erneut geöffnet. Der Auftrag bleibt gespeichert."
                : "Die Zielversion wurde noch nicht vollständig installiert. Mit Installation fortsetzen wird zuerst erneut geprüft und bei Bedarf der Installer geöffnet. Halte die DAWs geschlossen. Der Auftrag bleibt gespeichert."
            button.title = "Installation fortsetzen"
            window?.makeKeyAndOrderFront(nil)
        } else {
            step = .verify
            button.title = "Installer anzeigen / prüfen"
            detail.stringValue = "Schließe den Vorgang im Installer ab. Nach einem Abbruch: Beende Installer; anschließend kannst du die Installation hier fortsetzen. Die DAWs müssen geschlossen bleiben."
        }
    }
}

// Explicit boundaries let the tests drive the real AppKit controller without
// opening Installer, touching installed plugins or contacting the network.
struct UpdaterOperations {
    var httpConfiguration: () -> URLSessionConfiguration = { .ephemeral }
    var confirmDeferredCompletion: (URL) -> Bool = { recoveryApp in
        let alert = NSAlert()
        alert.messageText = "Update später abschließen?"
        alert.informativeText = "Der Auftrag ist gespeichert. Die Prüfung und Sicherung werden beim nächsten Start über Updates im Plugin fortgesetzt. Ein laufender macOS-Installer wird dadurch nicht beendet. Du kannst auch nur das Fenster schließen; dann prüft der Updater weiter.\n\nWiederaufnahme ohne DAW: " + recoveryApp.path
        alert.addButton(withTitle: "Weiter prüfen")
        alert.addButton(withTitle: "Später fortsetzen")
        return alert.runModal() == .alertSecondButtonReturn
    }
    var saveRecoveryHelper: (URL, Product) throws -> Void = { destination, product in
        try rejectSymlinkAncestors(destination)
        if !FileManager.default.fileExists(atPath: destination.path) {
            do { try FileManager.default.copyItem(at: Bundle.main.bundleURL, to: destination) }
            catch { try? FileManager.default.removeItem(at: destination); throw error }
        }
        let info = try PropertyListSerialization.propertyList(from: Data(contentsOf: destination.appendingPathComponent("Contents/Info.plist")), format: nil) as? [String: Any]
        try require(info?["WKProduct"] as? String == product.rawValue, "Gesicherter Updater gehört zu einem anderen Plugin")
        try verifiedTool("/usr/bin/codesign", ["--verify", "--deep", "--strict", destination.path], message: "Der Updater für die Wiederaufnahme konnte nicht gesichert werden")
    }
    var prepare: (URL, UpdateCandidate, Product, URL) throws -> PreparedPackage = {
        try PackageService.prepare($0, candidate: $1, product: $2, workspace: $3)
    }
    var installed: (PreparedPackage, Product) throws -> Bool = { try PackageService.installed($0, product: $1) }
    var usersOfPlugin: (Product) throws -> Set<Int32> = PackageService.usersOfPlugin
    var rejectDowngrade: (Product, Version) throws -> Void = { try PackageService.rejectDowngrade($0, target: $1) }
    var finishUserMigration: (Product, UserCopy?, String) throws -> URL? = { product, copy, id in
        if let copy = copy {
            let root = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Whykiki Audio/Update Backups/" + product.rawValue + "/" + id)
            return try copy.archive(to: root)
        }
        try rejectSymlinkAncestors(product.userBundle)
        try require(!FileManager.default.fileExists(atPath: product.userBundle.path),
                    "Zwischenzeitlich wurde eine Benutzerinstallation angelegt. Bitte diese Installation prüfen; sie könnte die neue Version verdecken.")
        return nil
    }
    var now: () -> Date = Date.init
    var installerRunning: () -> Bool = { !NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.installer").isEmpty }
    var activateInstaller: () -> Void = {
        NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.installer").first?.activate(options: [])
    }
    var openInstaller: (URL, @escaping (Error?) -> Void) -> Void = { package, completion in
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = true
        NSWorkspace.shared.open([package], withApplicationAt: URL(fileURLWithPath: "/System/Library/CoreServices/Installer.app"),
                                configuration: configuration) { _, error in completion(error) }
    }
}
