import AppKit
import Sparkle

private let canvas = NSColor(srgbRed: 18/255, green: 19/255, blue: 21/255, alpha: 1)
private let border = NSColor(white: 0.21, alpha: 1)
private let muted = NSColor(white: 0.68, alpha: 1)

private func caption(_ text: String, size: CGFloat = 11, color: NSColor = muted) -> NSTextField {
    let field = NSTextField(labelWithAttributedString: NSAttributedString(string: text, attributes: [
        .font: NSFont.monospacedSystemFont(ofSize: size, weight: .semibold),
        .kern: 1.15, .foregroundColor: color
    ]))
    return field
}

private final class Wordmark: NSView {
    private let art: String = {
        guard let file = Bundle.main.url(forResource: "ascii-font", withExtension: "txt"),
              let text = try? String(contentsOf: file, encoding: .utf8) else { return "HERMES" }
        return text.trimmingCharacters(in: .newlines)
    }()
    override func draw(_ dirtyRect: NSRect) {
        let lines = art.components(separatedBy: .newlines)
        let sample = NSFont.monospacedSystemFont(ofSize: 16, weight: .regular)
        let unit = ("M" as NSString).size(withAttributes: [.font: sample]).width
        let cols = CGFloat(lines.map(\.count).max() ?? 1)
        let size = min(20, 16 * (bounds.width - 48) / max(1, cols * unit))
        let font = NSFont.monospacedSystemFont(ofSize: max(1, size), weight: .regular)
        let attributes: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.white, .ligature: 0]
        let width = ("M" as NSString).size(withAttributes: attributes).width * cols
        let leading = ceil(font.ascender - font.descender)
        let top = bounds.midY + CGFloat(lines.count) * leading / 2
        for (index, line) in lines.enumerated() {
            (line as NSString).draw(at: .init(x: round(bounds.midX - width / 2), y: round(top - CGFloat(index + 1) * leading)), withAttributes: attributes)
        }
    }
}

private final class Hermes: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private var controller: SPUStandardUpdaterController!
    private let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"

    func applicationDidFinishLaunching(_ notification: Notification) {
        controller = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: nil, userDriverDelegate: nil)
        let menu = NSMenu()
        let appItem = menu.addItem(withTitle: "HERMES", action: nil, keyEquivalent: "")
        let appMenu = NSMenu(title: "HERMES")
        appItem.submenu = appMenu
        appMenu.addItem(withTitle: "About HERMES", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(withTitle: "Check for Updates…", action: #selector(SPUStandardUpdaterController.checkForUpdates(_:)), keyEquivalent: "").target = controller
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit HERMES", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        NSApp.mainMenu = menu

        let visible = NSScreen.main?.visibleFrame.size ?? NSSize(width: 1180, height: 900)
        window = NSWindow(contentRect: .init(x: 0, y: 0, width: min(1180, visible.width), height: min(900, visible.height - 32)),
                          styleMask: [.titled, .closable, .resizable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "HERMES"
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = canvas
        window.minSize = .init(width: 490, height: 320)
        guard let root = window.contentView else { return }

        func rule() -> NSView {
            let view = NSView()
            view.wantsLayer = true
            view.layer?.backgroundColor = border.cgColor
            return view
        }
        let title = NSTextField(labelWithString: "HERMES")
        title.font = .systemFont(ofSize: 20, weight: .heavy)
        title.textColor = .white
        let divider = rule()
        let badge = caption(" DESKTOP ", color: .black)
        badge.drawsBackground = true
        badge.backgroundColor = .white
        let headerRule = rule()
        let footerRule = rule()
        let refresh = NSButton(image: NSImage(systemSymbolName: "arrow.clockwise", accessibilityDescription: "Check for Updates")!, target: controller, action: #selector(SPUStandardUpdaterController.checkForUpdates(_:)))
        refresh.isBordered = false
        refresh.contentTintColor = muted
        refresh.toolTip = "Check for Updates…"
        let refreshDivider = rule()
        let wordmark = Wordmark()
        wordmark.setAccessibilityElement(true)
        wordmark.setAccessibilityRole(.staticText)
        wordmark.setAccessibilityLabel("HERMES")
        let build = caption("VERSION " + version)
        let platform = caption("MACOS")
        for item in [title, divider, badge, refreshDivider, refresh, headerRule, wordmark, footerRule, build, platform] {
            item.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(item)
        }
        NSLayoutConstraint.activate([
            headerRule.topAnchor.constraint(equalTo: root.topAnchor, constant: 48), headerRule.heightAnchor.constraint(equalToConstant: 1),
            headerRule.leadingAnchor.constraint(equalTo: root.leadingAnchor), headerRule.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            title.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 20), title.centerYAnchor.constraint(equalTo: root.topAnchor, constant: 24),
            divider.leadingAnchor.constraint(equalTo: title.trailingAnchor, constant: 20), divider.topAnchor.constraint(equalTo: root.topAnchor),
            divider.bottomAnchor.constraint(equalTo: headerRule.topAnchor), divider.widthAnchor.constraint(equalToConstant: 1),
            badge.leadingAnchor.constraint(equalTo: divider.trailingAnchor, constant: 16), badge.centerYAnchor.constraint(equalTo: title.centerYAnchor),
            refresh.trailingAnchor.constraint(equalTo: root.trailingAnchor), refresh.topAnchor.constraint(equalTo: root.topAnchor),
            refresh.widthAnchor.constraint(equalToConstant: 48), refresh.bottomAnchor.constraint(equalTo: headerRule.topAnchor),
            refreshDivider.trailingAnchor.constraint(equalTo: refresh.leadingAnchor), refreshDivider.widthAnchor.constraint(equalToConstant: 1),
            refreshDivider.topAnchor.constraint(equalTo: root.topAnchor), refreshDivider.bottomAnchor.constraint(equalTo: headerRule.topAnchor),
            footerRule.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -32), footerRule.heightAnchor.constraint(equalToConstant: 1),
            footerRule.leadingAnchor.constraint(equalTo: root.leadingAnchor), footerRule.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            wordmark.topAnchor.constraint(equalTo: headerRule.bottomAnchor), wordmark.bottomAnchor.constraint(equalTo: footerRule.topAnchor),
            wordmark.leadingAnchor.constraint(equalTo: root.leadingAnchor), wordmark.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            build.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 20), build.centerYAnchor.constraint(equalTo: root.bottomAnchor, constant: -16),
            platform.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -20), platform.centerYAnchor.constraint(equalTo: build.centerYAnchor)
        ])
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

let application = NSApplication.shared
private let delegate = Hermes()
application.setActivationPolicy(.regular)
application.delegate = delegate
application.run()
