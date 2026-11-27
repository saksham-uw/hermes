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
    @objc private func about() {
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
        driver.showAbout(version: version)
    }
    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool { menuItem.action == #selector(about) ? driver.panel == nil : driver.panel != nil || updater.canCheckForUpdates }

    func applicationDidFinishLaunching(_ notification: Notification) {
        Theme.registerFonts()
        updater = SPUUpdater(hostBundle: .main, applicationBundle: .main, userDriver: driver, delegate: nil)
        let menu = NSMenu()
        let sub = NSMenu(title: "HERMES")
        menu.addItem(withTitle: "HERMES", action: nil, keyEquivalent: "").submenu = sub
        sub.addItem(withTitle: "About HERMES", action: #selector(about), keyEquivalent: "").target = self
        sub.addItem(withTitle: "Check for Updates…", action: #selector(check), keyEquivalent: "").target = self
        sub.addItem(.separator())
        sub.addItem(withTitle: "Quit HERMES", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let edit = NSMenu(title: "Edit")
        menu.addItem(withTitle: "Edit", action: nil, keyEquivalent: "").submenu = edit
        for (title, selector, shortcut) in [("Cut", "cut:", "x"), ("Copy", "copy:", "c"),
                                            ("Paste", "paste:", "v"), ("Select All", "selectAll:", "a")] {
            edit.addItem(withTitle: title, action: NSSelectorFromString(selector), keyEquivalent: shortcut)
        }
        let windowMenu = NSMenu(title: "Window")
        menu.addItem(withTitle: "Window", action: nil, keyEquivalent: "").submenu = windowMenu
        windowMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowMenu.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        NSApp.mainMenu = menu

        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"
        window = HermesAppWindow(contentRect: NSRect(x: 0, y: 0, width: 980, height: 620),
            styleMask: [.titled, .closable, .resizable, .miniaturizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.title = "HERMES"
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.titlebarSeparatorStyle = .none
        for button: NSWindow.ButtonType in [.closeButton, .miniaturizeButton, .zoomButton] {
            window.standardWindowButton(button)?.isHidden = true
        }
        window.acceptsMouseMovedEvents = true
        window.appearance = NSAppearance(named: .darkAqua)
        let content = NSHostingView(rootView: HermesWindow(updatePreferences: UpdatePreferences(updater), updates: driver, version: version, check: check))
        content.sizingOptions = []
        window.contentView = content
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
