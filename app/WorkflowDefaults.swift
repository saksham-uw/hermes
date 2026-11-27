import SwiftUI

struct WorkflowDefaultsPage: View {
    @State private var github: Connection?
    @State private var slack: Connection?
    @State private var problem = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 22) {
            if let github {
                ReviewerPreference(connection: github)
            } else { Text("Connect GitHub to set a default reviewer.") }
            Divider()
            if let slack {
                RecipientPreference(connection: slack)
            } else { Text("Connect Slack to set a direct-message recipient.") }
            if let github {
                Divider()
                RuntimePreference(connection: github)
            }
            if !problem.isEmpty { Text(problem) }
        }.onAppear {
            do {
                github = try ConnectionStore().read(.github)
                slack = try ConnectionStore().read(.slack)
            } catch { problem = error.localizedDescription }
        }
    }
}

enum WorkflowKeys {
    static func reviewer(_ c: Connection) -> String { "workflow.review.\(c.scopeID).\(c.user)" }
    static func recipient(_ c: Connection) -> String { "workflow.dm.\(c.scopeID).\(c.user)" }
    static func reviewers(_ text: String) -> [String] {
        var seen = Set<String>()
        return text.split { $0 == "," || $0.isWhitespace }.map { String($0.drop(while: { $0 == "@" })) }
            .filter { !$0.isEmpty && seen.insert($0.lowercased()).inserted }
    }
}

private struct ReviewerPreference: View {
    let connection: Connection
    @AppStorage private var reviewers: String
    init(connection: Connection) {
        self.connection = connection
        _reviewers = AppStorage(wrappedValue: "", WorkflowKeys.reviewer(connection))
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            Text("GitHub reviewers").font(Theme.display(17))
            TextField("GitHub usernames, separated by commas", text: $reviewers)
            Text("Requested when you open a PR into main or prod for \(connection.scopeID).")
                .foregroundColor(Theme.muted)
        }
    }
}

private struct RuntimePreference: View {
    @AppStorage private var context: String
    init(connection: Connection) {
        _context = AppStorage(wrappedValue: "", "workflow.docker." + connection.scopeID)
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            Text("Production runtime").font(Theme.display(17))
            TextField("Production Docker context (optional)", text: $context)
            Text("Reads running container images from this Docker context. Use a context that points to your production host.").foregroundColor(Theme.muted)
        }
    }
}

private struct RecipientPreference: View {
    let connection: Connection
    @AppStorage private var recipient: String
    @State private var people: [ConnectorAPI.Member] = []
    @State private var problem = ""
    init(connection: Connection) {
        self.connection = connection
        _recipient = AppStorage(wrappedValue: "", WorkflowKeys.recipient(connection))
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            Text("Slack recipient").font(Theme.display(17))
            if !people.isEmpty {
                Menu(people.first(where: { $0.id == recipient })?.label ?? "Choose a workspace member") {
                    ForEach(people) { person in Button(person.label + " · @" + person.name) { recipient = person.id } }
                }
            }
            TextField("Slack member ID or existing DM link", text: $recipient)
            Text("PR links go to this person in \(connection.scope) when you click Send on Slack.").foregroundColor(Theme.muted)
            if !recipient.isEmpty && ConnectorAPI.recipientID(recipient) == nil { Text("Enter a member ID or a one-to-one Slack DM link.") }
            if !problem.isEmpty { Text(problem).foregroundColor(Theme.muted) }
        }.task {
            do { people = try await ConnectorAPI().members(connection) }
            catch { problem = error.localizedDescription.contains("missing_scope")
                ? "Reconnect Slack for the member picker, or paste an existing DM link."
                : error.localizedDescription }
        }
    }
}
