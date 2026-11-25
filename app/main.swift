import AppKit
import SwiftUI
import Sparkle

private final class Hermes: NSObject, NSApplicationDelegate, NSMenuItemValidation {
    private let driver = UpdateDriver()
    private var updater: SPUUpdater!
    private var window: NSWindow!
    @objc private func check() {
        if driver.panel != nil { driver.showUpdateInFocus() }
        else if updater.canCheckForUpdates { updater.checkForUpdates() }
    }
    func validateMenuItem(_ item: NSMenuItem) -> Bool { driver.panel != nil || updater.canCheckForUpdates }

    func applicationDidFinishLaunching(_ notification: Notification) {
        Theme.registerFonts()
        updater = SPUUpdater(hostBundle: .main, applicationBundle: .main, userDriver: driver, delegate: nil)
        let menu = NSMenu()
        let sub = NSMenu(title: "HERMES")
        menu.addItem(withTitle: "HERMES", action: nil, keyEquivalent: "").submenu = sub
        sub.addItem(withTitle: "About HERMES", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        sub.addItem(withTitle: "Check for Updates…", action: #selector(check), keyEquivalent: "").target = self
        sub.addItem(.separator())
        sub.addItem(withTitle: "Quit HERMES", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        NSApp.mainMenu = menu

        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 980, height: 620),
            styleMask: [.titled, .closable, .resizable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "HERMES"
        window.appearance = NSAppearance(named: .darkAqua)
        window.contentView = NSHostingView(rootView: Workspace(updates: driver, version: version, check: check))
        window.contentMinSize = NSSize(width: 500, height: 320)
        window.center()
        window.makeKeyAndOrderFront(nil)
        driver.window = window
        NSApp.activate(ignoringOtherApps: true)
        do {
            try updater.start()
            if updater.automaticallyChecksForUpdates { updater.checkForUpdatesInBackground() }
        } catch { driver.showUpdaterError(error) {} }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
let application = NSApplication.shared
private let delegate = Hermes()
application.setActivationPolicy(.regular)
application.delegate = delegate
application.run()
