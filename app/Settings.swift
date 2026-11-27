import SwiftUI
import Sparkle

// Sparkle owns the preference and reschedules its timer when it changes.
final class UpdatePreferences: ObservableObject {
    private let updater: SPUUpdater
    @Published private(set) var automaticChecks: Bool

    init(_ updater: SPUUpdater) {
        self.updater = updater
        automaticChecks = updater.automaticallyChecksForUpdates
        updater.publisher(for: \.automaticallyChecksForUpdates).assign(to: &$automaticChecks)
    }

    func setAutomaticChecks(_ enabled: Bool) {
        updater.automaticallyChecksForUpdates = enabled
    }
}

enum SettingsSection: String, CaseIterable {
    case appearance = "Appearance"
    case connectors = "Connectors"
    case workflow = "Workflow defaults"
    case updates = "Updates"
}

struct HermesToggleStyle: ToggleStyle {
    func makeBody(configuration: Configuration) -> some View {
        Button {
            configuration.isOn.toggle()
        } label: {
            HStack(spacing: 12) {
                configuration.label
                HStack(spacing: 5) {
                    if configuration.isOn { LabelText(text: "On", bold: true) }
                    Rectangle()
                        .fill(configuration.isOn ? Theme.canvas : Theme.muted)
                        .frame(width: 9, height: 14)
                    if !configuration.isOn { LabelText(text: "Off", bold: true) }
                }
                .frame(width: 58, height: 24)
                .foregroundColor(configuration.isOn ? Theme.canvas : Theme.muted)
                .background(configuration.isOn ? Theme.ink : Theme.canvas)
                .overlay(Rectangle().strokeBorder(configuration.isOn ? Theme.ink : Theme.muted))
                .accessibilityHidden(true)
            }
            .contentShape(Rectangle()).textSelection(.disabled)
        }
        .buttonStyle(.plain).modifier(ControlHover())
        .accessibilityRepresentation {
            Toggle(isOn: configuration.$isOn) { configuration.label }
                .toggleStyle(.switch)
        }
    }
}

struct SettingsPage: View {
    @Binding var section: SettingsSection
    @Binding var showGrid: Bool
    @ObservedObject var updates: UpdatePreferences
    let version: String
    let check: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            VStack(alignment: .leading, spacing: 5) {
                LabelText(text: "Settings").foregroundColor(Theme.muted)
                    .padding(.leading, 12).padding(.bottom, 10)
                ForEach(SettingsSection.allCases, id: \.self) { item in
                    let selected = item == section
                    Button { section = item } label: {
                        HStack(spacing: 10) {
                            SettingsGlyph(section: item).frame(width: 17, height: 17)
                            Text(item.rawValue).font(.custom(selected ? "Inter-SemiBold" : "Inter-Regular", size: 13))
                                .lineLimit(1).frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .padding(.horizontal, 12).frame(height: 38)
                        .foregroundColor(selected ? Theme.ink : Theme.muted)
                        .background(RoundedRectangle(cornerRadius: 4).fill(selected ? Color(white: 0.215) : Color.clear))
                        .overlay(alignment: .leading) {
                            Rectangle().fill(selected ? Theme.ink : Color.clear).frame(width: 2, height: 17)
                        }.textSelection(.disabled).contentShape(Rectangle())
                    }.buttonStyle(.plain).modifier(ControlHover())
                     .accessibilityAddTraits(selected ? .isSelected : [])
                }
                Spacer(minLength: 0)
            }.padding(.horizontal, 10).padding(.vertical, 22).frame(width: 182)
             .background(Color(white: 0.145))
            Rectangle().fill(Theme.rule).frame(width: 1)
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    Text(section.rawValue).font(Theme.display(26)).accessibilityAddTraits(.isHeader)
                    Rectangle().fill(Theme.rule).frame(height: 1)
                    switch section {
                    case .appearance:
                        preference("Workspace grid", "Show the dot grid in Workspace.", enabled: $showGrid)
                    case .connectors:
                        ConnectorsPage()
                    case .workflow:
                        WorkflowDefaultsPage()
                    case .updates:
                        preference("Automatic update checks", "Check for new releases when HERMES opens and hourly while it’s running.",
                                   enabled: Binding(get: { updates.automaticChecks }, set: updates.setAutomaticChecks))
                        Rectangle().fill(Theme.rule).frame(height: 1)
                        LabelText(text: "Installed version  " + version).foregroundColor(Theme.muted)
                        Button(action: check) { LabelText(text: "Check for updates", bold: true) }.buttonStyle(ActionStyle())
                    }
                }.padding(24).frame(maxWidth: .infinity, alignment: .leading)
            }.frame(maxWidth: .infinity).background(Theme.surface)
        }.frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func preference(_ title: String, _ detail: String, enabled: Binding<Bool>) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Toggle(isOn: enabled) {
                Text(title).font(.custom("Inter-SemiBold", size: 16)).frame(maxWidth: .infinity, alignment: .leading)
            }.toggleStyle(HermesToggleStyle())
            Text(detail).font(.custom("Inter-Regular", size: 14)).foregroundColor(Theme.muted)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

// Small line icons share a 16-point grid and the foreground of their row.
private struct SettingsGlyph: View {
    let section: SettingsSection
    var body: some View {
        Canvas { context, size in
            context.scaleBy(x: size.width / 16, y: size.height / 16)
            var path = Path()
            func line(_ x: CGFloat, _ y: CGFloat, _ u: CGFloat, _ v: CGFloat) {
                path.move(to: CGPoint(x: x, y: y))
                path.addLine(to: CGPoint(x: u, y: v))
            }
            switch section {
            case .appearance:
                path.addEllipse(in: CGRect(x: 2, y: 2, width: 12, height: 12))
                var half = Path()
                half.addArc(center: CGPoint(x: 8, y: 8), radius: 5.5, startAngle: .degrees(90), endAngle: .degrees(270), clockwise: false)
                half.closeSubpath()
                context.fill(half, with: .foreground)
            case .connectors:
                path.addRoundedRect(in: CGRect(x: 1.5, y: 6.5, width: 8, height: 7), cornerSize: CGSize(width: 2, height: 2))
                path.addRoundedRect(in: CGRect(x: 6.5, y: 2.5, width: 8, height: 7), cornerSize: CGSize(width: 2, height: 2))
            case .workflow:
                for (y, x) in [(4.0, 6.0), (8.0, 10.0), (12.0, 5.0)] {
                    line(2, y, x - 2, y); line(x + 2, y, 14, y)
                    path.addEllipse(in: CGRect(x: x - 1.5, y: y - 1.5, width: 3, height: 3))
                }
            case .updates:
                path.addArc(center: CGPoint(x: 8, y: 8), radius: 5.5, startAngle: .degrees(-45), endAngle: .degrees(255), clockwise: false)
                line(12.4, 1.7, 12.4, 5.2); line(12.4, 5.2, 8.9, 5.2)
            }
            context.stroke(path, with: .foreground, style: StrokeStyle(lineWidth: 1.2, lineCap: .round, lineJoin: .round))
        }.accessibilityHidden(true)
    }
}
