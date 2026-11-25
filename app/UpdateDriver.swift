import AppKit
import Combine
import Sparkle

struct PanelAction {
    let label: String
    let run: () -> Void
    init(_ label: String, _ run: @escaping () -> Void) { self.label = label; self.run = run }
}

struct UpdatePanel {
    let title: String
    let message: String
    var primary: PanelAction? = nil
    var secondary: PanelAction? = nil
    var progress: Double? = nil
    var releaseURL: URL? = nil
}

final class UpdateDriver: NSObject, ObservableObject, SPUUserDriver {
    @Published var panel: UpdatePanel?
    weak var window: NSWindow?
    private var received: UInt64 = 0
    private var expected: UInt64 = 0
    private var acceptedRestart = false

    private func action(_ label: String, _ callback: @escaping () -> Void) -> PanelAction {
        PanelAction(label) { [weak self] in
            self?.panel = nil
            callback()
        }
    }

    func show(_ request: SPUUpdatePermissionRequest, reply: @escaping (SUUpdatePermissionResponse) -> Void) {
        panel = UpdatePanel(title: "Check automatically?", message: "HERMES can periodically check for a new version.",
            primary: action("Enable") { reply(.init(automaticUpdateChecks: true, sendSystemProfile: false)) },
            secondary: action("Later") { reply(.init(automaticUpdateChecks: false, sendSystemProfile: false)) })
    }

    func showUserInitiatedUpdateCheck(cancellation: @escaping () -> Void) {
        panel = UpdatePanel(title: "Checking for updates…", message: "Looking for the latest HERMES release.", secondary: action("Cancel", cancellation))
    }

    func showUpdateFound(with appcastItem: SUAppcastItem, state: SPUUserUpdateState, reply: @escaping (SPUUserUpdateChoice) -> Void) {
        acceptedRestart = false
        let primary: PanelAction
        if appcastItem.isInformationOnlyUpdate {
            primary = action("View release") {
                if let url = appcastItem.infoURL { NSWorkspace.shared.open(url) }
                reply(.dismiss)
            }
        } else {
            primary = action("Update and restart") { [weak self] in
                self?.acceptedRestart = true
                reply(.install)
            }
        }
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
        panel = UpdatePanel(title: "HERMES \(appcastItem.displayVersionString) is available.",
            message: "You’re running \(version). " + (appcastItem.isInformationOnlyUpdate ? "View the release for details." : "Install the update and reopen HERMES."),
            primary: primary, secondary: action("Later") { reply(.dismiss) },
            releaseURL: URL(string: "https://github.com/saksham-uw/hermes/releases")?.appendingPathComponent("tag/v" + appcastItem.displayVersionString))
    }

    func showUpdateReleaseNotes(with downloadData: SPUDownloadData) {}
    func showUpdateReleaseNotesFailedToDownloadWithError(_ error: Error) {}
    func showUpdateNotFoundWithError(_ error: Error, acknowledgement: @escaping () -> Void) {
        panel = UpdatePanel(title: "You’re up to date.", message: error.localizedDescription, primary: action("Done", acknowledgement))
    }
    func showUpdaterError(_ error: Error, acknowledgement: @escaping () -> Void) {
        panel = UpdatePanel(title: "Update interrupted.", message: error.localizedDescription, primary: action("Close", acknowledgement))
    }
    func showDownloadInitiated(cancellation: @escaping () -> Void) {
        expected = 0
        received = 0
        panel = UpdatePanel(title: "Downloading update…", message: "HERMES will verify the download before installing it.", secondary: action("Cancel", cancellation))
    }
    func showDownloadDidReceiveExpectedContentLength(_ length: UInt64) { expected = length }
    func showDownloadDidReceiveData(ofLength length: UInt64) {
        received += length
        if expected > 0 { panel?.progress = min(1, Double(received) / Double(expected)) }
    }
    func showDownloadDidStartExtractingUpdate() {
        panel = UpdatePanel(title: "Preparing update…", message: "Unpacking the new version.", progress: 0)
    }
    func showExtractionReceivedProgress(_ progress: Double) { panel?.progress = progress }
    func showReady(toInstallAndRelaunch reply: @escaping (SPUUserUpdateChoice) -> Void) {
        if acceptedRestart { reply(.install); return }
        panel = UpdatePanel(title: "Ready to restart.", message: "Reopen HERMES to finish installing the update.",
            primary: action("Restart") { reply(.install) }, secondary: action("Later") { reply(.dismiss) })
    }
    func showInstallingUpdate(withApplicationTerminated terminated: Bool, retryTerminatingApplication: @escaping () -> Void) {
        panel = UpdatePanel(title: "Restarting HERMES…", message: "The new version will open automatically.",
            primary: terminated ? nil : action("Retry restart", retryTerminatingApplication))
    }
    func showUpdateInstalledAndRelaunched(_ relaunched: Bool, acknowledgement: @escaping () -> Void) { acknowledgement() }
    func dismissUpdateInstallation() { panel = nil; acceptedRestart = false }
    func showUpdateInFocus() { window?.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true) }
}
