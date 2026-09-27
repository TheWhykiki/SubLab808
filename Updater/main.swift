import AppKit

let application = NSApplication.shared
let delegate = UpdaterApp()
application.delegate = delegate
application.setActivationPolicy(.regular)
application.run()
