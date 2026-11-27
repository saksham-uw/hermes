import SwiftUI
import CoreText

enum Theme {
    static let canvas = Color(white: 0.167)
    static let surface = Color(white: 0.205)
    static let ink = Color(white: 0.94)
    static let muted = Color(white: 0.72)
    static let rule = Color.white.opacity(0.15)

    static func registerFonts() {
        for name in ["Archivo-Variable", "GeistMono-Variable", "Inter-Variable"] {
            if let url = Bundle.main.url(forResource: name, withExtension: "ttf", subdirectory: "Fonts") {
                CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
            }
        }
    }

    static func display(_ size: CGFloat) -> Font {
        let descriptor = CTFontDescriptorCreateWithAttributes([
            kCTFontNameAttribute: "Archivo-SemiBold",
            kCTFontVariationAttribute: [0x77676874: 620, 0x77647468: 118] // wght, wdth
        ] as CFDictionary)
        return Font(CTFontCreateWithFontDescriptor(descriptor, size, nil))
    }
}

struct LabelText: View {
    let text: String
    var bold = false
    var body: some View {
        Text(verbatim: text.uppercased()).font(.custom(bold ? "GeistMono-Bold" : "GeistMono-Medium", size: 11))
            .tracking(bold ? 1.32 : 1.65).lineLimit(1)
    }
}

struct ActionStyle: ButtonStyle {
    var primary = false
    @Environment(\.isEnabled) private var isEnabled
    @Environment(\.isFocused) private var isFocused
    init(primary: Bool = false) { self.primary = primary }
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.textSelection(.disabled).padding(.horizontal, 14).frame(minHeight: 34)
            .foregroundColor(primary ? Theme.canvas : Theme.ink)
            .background(RoundedRectangle(cornerRadius: 4).fill(primary ? Theme.ink : Theme.surface))
            .overlay(RoundedRectangle(cornerRadius: 4).strokeBorder(isFocused || primary ? Theme.ink : Theme.rule))
            .opacity(isEnabled ? (configuration.isPressed ? 0.66 : 1) : 0.45)
            .contentShape(Rectangle())
    }
}

struct TabStyle: ButtonStyle {
    let selected: Bool
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.textSelection(.disabled).padding(.horizontal, 11).frame(minHeight: 34)
            .foregroundColor(selected ? Theme.ink : Theme.muted)
            .overlay(alignment: .bottom) {
                Rectangle().fill(selected ? Theme.ink : Color.clear).frame(height: 2)
            }.opacity(configuration.isPressed ? 0.65 : 1).contentShape(Rectangle())
    }
}

private enum MainTab: String, CaseIterable {
    case workspace = "Workspace"
    case settings = "Settings"
}

struct HermesWindow: View {
    @State private var tab: MainTab = .workspace
    @State private var section: SettingsSection = .appearance
    @AppStorage("workspace.showGrid") private var showGrid = true
    let updatePreferences: UpdatePreferences
    @ObservedObject var updates: UpdateDriver
    let version: String
    let check: () -> Void

    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                HStack(spacing: 12) {
                    Text("HERMES").font(Theme.display(22)).tracking(-0.8)
                    Rectangle().fill(Theme.rule).frame(width: 1, height: 24)
                    HStack(spacing: 0) {
                        ForEach(MainTab.allCases, id: \.self) { item in
                            Button { tab = item } label: { LabelText(text: item.rawValue, bold: true) }
                                .buttonStyle(TabStyle(selected: tab == item))
                                .accessibilityAddTraits(tab == item ? .isSelected : [])
                                .keyboardShortcut(item == .workspace ? "1" : ",", modifiers: .command)
                        }
                    }.fixedSize()
                    DragSurface().frame(maxWidth: .infinity, maxHeight: .infinity)
                    LabelText(text: "v" + version).foregroundColor(Theme.muted)
                    WindowButtons()
                }.padding(.horizontal, 18).frame(height: 54)
                Rectangle().fill(Theme.rule).frame(height: 1)
                switch tab {
                case .workspace:
                    RepositoryWorkspace(showGrid: showGrid) { destination in
                        section = destination; tab = .settings
                    }
                case .settings:
                    SettingsPage(section: $section, showGrid: $showGrid, updates: updatePreferences, version: version, check: check)
                }
            }.disabled(updates.panel != nil).accessibilityHidden(updates.panel != nil)
            if let panel = updates.panel {
                Color.black.opacity(0.42).ignoresSafeArea()
                VStack(alignment: .leading, spacing: 0) {
                    HStack {
                        LabelText(text: "HERMES").foregroundColor(Theme.ink)
                        Spacer()
                        LabelText(text: "Software update").foregroundColor(Theme.muted)
                    }.padding(.horizontal, 22).frame(height: 39)
                    Rectangle().fill(Theme.rule).frame(height: 1)
                    VStack(alignment: .leading, spacing: 10) {
                        Text(panel.title).font(Theme.display(26)).fixedSize(horizontal: false, vertical: true)
                        Text(panel.message).font(.custom("Inter-Regular", size: 14)).foregroundColor(Theme.muted)
                            .fixedSize(horizontal: false, vertical: true)
                        if let progress = panel.progress {
                            GeometryReader { geo in
                                Rectangle().fill(Theme.rule).overlay(alignment: .leading) {
                                    Rectangle().fill(Theme.ink).frame(width: geo.size.width * min(max(progress, 0), 1))
                                }
                            }.frame(height: 3).accessibilityLabel("Update progress").accessibilityValue("\(Int(progress * 100)) percent")
                        }
                    }.padding(22)
                    if panel.primary != nil || panel.secondary != nil {
                        HStack(spacing: 8) {
                            if let link = panel.releaseURL {
                                actionButton("What’s new") { NSWorkspace.shared.open(link) }
                            }
                            Spacer()
                            if let action = panel.secondary { Button(action: action.run) { LabelText(text: action.label, bold: true) }.buttonStyle(ActionStyle()).keyboardShortcut(.cancelAction) }
                            if let action = panel.primary { Button(action: action.run) { LabelText(text: action.label, bold: true) }.buttonStyle(ActionStyle(primary: true)).keyboardShortcut(.defaultAction) }
                        }.padding([.horizontal, .bottom], 22)
                    }
                }.background(Theme.surface).overlay(Rectangle().strokeBorder(Theme.rule))
                 .shadow(color: .black.opacity(0.3), radius: 14, y: 6).frame(maxWidth: 496).padding(.horizontal, 24)
            }
        }.foregroundColor(Theme.ink).background(Theme.canvas).preferredColorScheme(.dark)
            .textSelection(.enabled).tint(Theme.ink).font(.custom("Inter-Regular", size: 14)).ignoresSafeArea()
    }
}

struct WorkspaceCanvas: View {
    let showGrid: Bool
    private static let artwork = Bundle.main.url(forResource: "ascii-font", withExtension: "txt")
        .flatMap { try? String(contentsOf: $0, encoding: .utf8) }?.trimmingCharacters(in: .newlines) ?? "HERMES"

    var body: some View {
        ZStack {
            Canvas { context, size in
                guard showGrid else { return }
                var dots = Path()
                for y in stride(from: 12, to: size.height, by: 20) {
                    for x in stride(from: 12, to: size.width, by: 20) {
                        dots.addRect(CGRect(x: x, y: y, width: 1, height: 1))
                    }
                }
                context.fill(dots, with: .color(.white.opacity(0.10)))
            }
            GeometryReader { proxy in
                Text(verbatim: Self.artwork)
                    .font(.custom("GeistMono-Regular", size: min(22, (proxy.size.width - 44) / 54, proxy.size.height / 17)))
                    .fixedSize().frame(width: proxy.size.width, height: proxy.size.height)
                    .accessibilityLabel("HERMES")
            }
        }.background(Theme.surface).overlay(Rectangle().strokeBorder(Theme.rule))
    }
}


private struct WindowButtons: View {
    var body: some View {
        HStack(spacing: 2) {
            control("Minimize", mark: "−") { NSApp.keyWindow?.miniaturize(nil) }
            control("Toggle full screen", mark: "□") { NSApp.keyWindow?.toggleFullScreen(nil) }
            control("Close window", mark: "×") { NSApp.keyWindow?.performClose(nil) }
        }
    }
    private func control(_ name: String, mark: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(verbatim: mark).font(.custom("GeistMono-Regular", size: 15)).frame(width: 23, height: 28)
                .contentShape(Rectangle()).textSelection(.disabled)
        }.buttonStyle(.plain).foregroundColor(Theme.muted).accessibilityLabel(name).help(name)
    }
}
