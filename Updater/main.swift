import AppKit
import Foundation
import Darwin

final class UpdaterApp: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private let status = NSTextField(wrappingLabelWithString: "")
    private let detail = NSTextField(wrappingLabelWithString: "")
    private let button = NSButton(title: "Nach Updates suchen", target: nil, action: nil)
    private let closeButton = NSButton(title: "Schließen", target: nil, action: nil)
    private let progress = NSProgressIndicator()
    private var product: Product!
    private var current: Version!
    private var host: NSRunningApplication?
    private var workspace: URL!
    private var userCopy: UserCopy?
    private var candidate: UpdateCandidate?
    private var prepared: PreparedPackage?
    private var client: HTTPClient?
    private var generation = 0
    private var timer: Timer?
    private var busy = false
    private var installerStarted = false
    private var checkingInstallation = false
    private var lockDescriptor: Int32 = -1
    private enum Step { case check, download, install, verify, finished }
    private var step = Step.check

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildWindow()
        do {
            guard let name = Bundle.main.object(forInfoDictionaryKey: "WKProduct") as? String,
                  let selected = Product(rawValue: name) else { throw UpdateFailure("Updater-Produktkonfiguration fehlt") }
            product = selected
            let arguments = CommandLine.arguments
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
            userCopy = try UserCopy.capture(product)
            let cache = try FileManager.default.url(for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
                .resolvingSymlinksInPath().appendingPathComponent("Whykiki Audio/Updates/" + product.rawValue, isDirectory: true)
            try rejectSymlinkAncestors(cache)
            try FileManager.default.createDirectory(at: cache, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            lockDescriptor = open(cache.appendingPathComponent("update.lock").path, O_RDWR | O_CREAT | O_NOFOLLOW, 0o600)
            try require(lockDescriptor >= 0 && flock(lockDescriptor, LOCK_EX | LOCK_NB) == 0, "Für dieses Plugin ist bereits ein Updater geöffnet.")
            workspace = cache.appendingPathComponent(UUID().uuidString, isDirectory: true)
            try FileManager.default.createDirectory(at: workspace, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            window.title = product.rawValue + " Update"
            detail.stringValue = "Installiert: \(current!) · Installationsziel: /Library/Audio/Plug-Ins/VST3\nDer Updater bleibt beim Schließen der DAW geöffnet."
            checkRelease()
        } catch { fail(error, retry: false) }
    }

    private func buildWindow() {
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

    @objc private func closeWindow() { NSApp.terminate(nil) }
    func windowShouldClose(_ sender: NSWindow) -> Bool { !checkingInstallation && !(busy && step == .install) }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Do not abandon the atomic backup operation or remove a package while opening Installer.
        if checkingInstallation || (busy && step == .install) { return .terminateCancel }
        return .terminateNow
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) {
        generation += 1
        client?.cancel()
        timer?.invalidate()
        if !installerStarted, let workspace = workspace { try? FileManager.default.removeItem(at: workspace) }
        if lockDescriptor >= 0 { flock(lockDescriptor, LOCK_UN); close(lockDescriptor) }
    }

    private func fail(_ error: Error, retry: Bool = true) {
        busy = false
        status.stringValue = "Update nicht abgeschlossen"
        detail.stringValue = error.localizedDescription
        button.isEnabled = retry
        button.title = installerStarted ? "Installation prüfen" : "Erneut versuchen"
        step = installerStarted ? .verify : .check
    }

    @objc private func primaryAction() {
        guard !busy else { return }
        switch step {
        case .check: checkRelease()
        case .download: download()
        case .install: beginInstallation()
        case .verify: verifyInstallation()
        case .finished: closeWindow()
        }
    }

    private func checkRelease() {
        guard let product = product, let current = current else { return }
        busy = true
        button.isEnabled = false
        status.stringValue = "Suche nach Updates …"
        generation += 1
        let token = generation
        client = HTTPClient(url: product.apiURL, limit: 1024 * 1024) { [weak self] result in
            guard let self = self, self.generation == token else { return }
            self.busy = false
            do {
                let release = try JSONDecoder().decode(Release.self, from: result.get())
                guard let candidate = try UpdateCandidate.select(release, product: product, current: current) else {
                    self.status.stringValue = "Du bist auf dem aktuellen Stand"
                    self.detail.stringValue = "Installiert: \(current). Es gibt keine neuere stabile Version für dieses Plugin."
                    self.step = .finished
                    self.button.title = "Fertig"
                    self.button.isEnabled = true
                    return
                }
                if let copy = self.userCopy { try require(copy.version < candidate.version, "Die Benutzerinstallation ist bereits gleich neu oder neuer. Sie wird nicht ersetzt.") }
                try PackageService.rejectDowngrade(product, target: candidate.version)
                self.candidate = candidate
                self.status.stringValue = "Version \(candidate.version) ist verfügbar"
                self.detail.stringValue = "Installiert: \(current) · Download: \(ByteCountFormatter.string(fromByteCount: Int64(candidate.size), countStyle: .file))\nDas Paket wird geprüft. Vor der Installation musst du dein Projekt speichern und alle DAWs schließen. macOS fragt bei Bedarf nach dem Administratorpasswort."
                self.step = .download
                self.button.title = "Update laden & installieren"
                self.button.isEnabled = true
            } catch { self.fail(error) }
        }
    }

    private func download() {
        guard let candidate = candidate else { return }
        busy = true
        button.isEnabled = false
        status.stringValue = "Update wird geladen …"
        let package = workspace.appendingPathComponent(product.packageName(candidate.version))
        try? FileManager.default.removeItem(at: package)
        let token = generation
        client = HTTPClient(url: candidate.url, destination: package, limit: candidate.size,
                            progress: { [weak self] value in self?.progress.doubleValue = value }) { [weak self] result in
            guard let self = self, self.generation == token else { return }
            do { _ = try result.get() } catch { self.fail(error); return }
            self.status.stringValue = "Prüfe Installationspaket …"
            let product = self.product!, workspace = self.workspace!
            DispatchQueue.global(qos: .userInitiated).async {
                let prepared = Result { try PackageService.prepare(package, candidate: candidate, product: product, workspace: workspace) }
                DispatchQueue.main.async {
                    guard self.generation == token else { return }
                    self.busy = false
                    switch prepared {
                    case .failure(let error): self.fail(error)
                    case .success(let package):
                        self.prepared = package
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

    private func beginInstallation() {
        guard let prepared = prepared else { return }
        guard host?.isTerminated != false else {
            detail.stringValue = "Die aufrufende DAW ist noch geöffnet. Bitte Projekt speichern und die DAW vollständig beenden. Dieser Updater bleibt geöffnet."
            return
        }
        busy = true
        button.isEnabled = false
        let product = product!
        DispatchQueue.global(qos: .userInitiated).async {
            let ready = Result {
                try PackageService.rejectDowngrade(product, target: prepared.candidate.version)
                try require(try PackageService.usersOfPlugin(product).isEmpty, "Eine weitere Anwendung verwendet dieses Plugin. Bitte alle DAWs schließen.")
                try prepared.candidate.verifyDownload(prepared.file)
            }
            DispatchQueue.main.async {
                do {
                    try ready.get()
                    let configuration = NSWorkspace.OpenConfiguration()
                    configuration.activates = true
                    NSWorkspace.shared.open([prepared.file], withApplicationAt: URL(fileURLWithPath: "/System/Library/CoreServices/Installer.app"),
                                            configuration: configuration) { _, error in
                        DispatchQueue.main.async {
                            self.busy = false
                            if let error = error { self.fail(error); return }
                            self.installerStarted = true
                            self.step = .verify
                            self.status.stringValue = "Installation in macOS abschließen"
                            self.detail.stringValue = "Folge dem geöffneten macOS-Installer. Die DAWs müssen geschlossen bleiben. Anschließend prüft dieser Updater Paketbeleg und installierte Dateien."
                            self.button.title = "Installation prüfen"
                            self.button.isEnabled = true
                            self.timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.verifyInstallation() }
                        }
                    }
                } catch { self.busy = false; self.button.isEnabled = true; self.detail.stringValue = error.localizedDescription }
            }
        }
    }

    private func verifyInstallation() {
        guard let prepared = prepared, !checkingInstallation else { return }
        checkingInstallation = true
        let product = product!, copy = userCopy
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { () -> String? in
                guard try PackageService.installed(prepared, product: product) else { return nil }
                try require(try PackageService.usersOfPlugin(product).isEmpty, "Bitte die DAWs schließen, bevor die Installation abgeschlossen wird.")
                var backup: URL?
                if let copy = copy {
                    let root = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Whykiki Audio/Update Backups/" + product.rawValue + "/" + UUID().uuidString)
                    backup = try copy.archive(to: root)
                } else {
                    try rejectSymlinkAncestors(product.userBundle)
                    try require(!FileManager.default.fileExists(atPath: product.userBundle.path),
                                "Zwischenzeitlich wurde eine Benutzerinstallation angelegt. Bitte diese Installation prüfen; sie könnte die neue Version verdecken.")
                }
                return "Version \(prepared.candidate.version) wurde geprüft und installiert. Starte die DAW neu; falls nötig, führe einen Plugin-Scan aus."
                    + (backup.map { "\nVorherige Benutzerinstallation: " + $0.path } ?? "")
            }
            DispatchQueue.main.async {
                self.checkingInstallation = false
                switch result {
                case .failure(let error): self.timer?.invalidate(); self.fail(error)
                case .success(nil): break
                case .success(let message?):
                    self.timer?.invalidate()
                    self.status.stringValue = "Update installiert"
                    self.detail.stringValue = message
                    self.userCopy = nil
                    self.step = .finished
                    self.button.title = "Fertig"
                    self.button.isEnabled = true
                }
            }
        }
    }
}

let application = NSApplication.shared
let delegate = UpdaterApp()
application.delegate = delegate
application.setActivationPolicy(.regular)
application.run()
