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
    case updates = "Updates"

    var icon: String {
        switch self {
        case .appearance: return "circle.lefthalf.filled"
        case .connectors: return "link"
        case .updates: return "arrow.triangle.2.circlepath"
        }
    }
}

private struct HermesToggleStyle: ToggleStyle {
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
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
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
            VStack(alignment: .leading, spacing: 4) {
                LabelText(text: "Settings").foregroundColor(Theme.muted).padding(.bottom, 14)
                ForEach(SettingsSection.allCases, id: \.self) { item in
                    Button { section = item } label: {
                        HStack(spacing: 9) {
                            Image(systemName: item.icon).frame(width: 16)
                            Text(item.rawValue).font(.custom("Inter-Regular", size: 14)).fixedSize()
                            Spacer(minLength: 0)
                        }.padding(10).contentShape(Rectangle())
                    }.buttonStyle(.plain)
                     .foregroundColor(item == section ? Theme.canvas : Theme.ink)
                     .background(item == section ? Theme.ink : Color.clear)
                     .accessibilityAddTraits(item == section ? .isSelected : [])
                }
                Spacer()
            }.padding(16).frame(width: 168)
            Rectangle().fill(Theme.rule).frame(width: 1)
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    Text(section.rawValue).font(Theme.display(26)).accessibilityAddTraits(.isHeader)
                    Rectangle().fill(Theme.rule).frame(height: 1)
                    switch section {
                    case .appearance:
                        preference("Workspace grid", "Show the dot grid behind the HERMES wordmark.", enabled: $showGrid)
                    case .connectors:
                        ConnectorsPage()
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
