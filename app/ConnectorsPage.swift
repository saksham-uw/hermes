import SwiftUI

private func actionButton(_ title: String, primary: Bool = false, action: @escaping () -> Void) -> some View {
    Button(action: action) { LabelText(text: title, bold: true) }.buttonStyle(ActionStyle(primary: primary))
}

private func entries(_ input: String) -> [String] {
    input.split(whereSeparator: { $0 == "," || $0.isWhitespace }).map(String.init)
}

struct ConnectorsPage: View {
    @State private var connections: [Service: Connection] = [:]
    @State private var selected: Service?
    @State private var error = ""
    private let store = ConnectionStore()

    var body: some View {
        ForEach(Service.allCases) { service in
            let connection = connections[service]
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 14) {
                    ConnectorMark(service: service)
                    VStack(alignment: .leading, spacing: 5) {
                        Text(service.rawValue).font(Theme.display(20))
                        Text(connection.map { "\($0.user) · \($0.scope)" } ?? "Disconnected")
                            .font(.custom("Inter-Regular", size: 13)).foregroundColor(Theme.muted)
                    }
                }
                Text(service == .slack ? "Send messages as yourself in one workspace." : "Manage branches and pull requests in selected repositories.")
                    .font(.custom("Inter-Regular", size: 14)).fixedSize(horizontal: false, vertical: true)
                actionButton(connection == nil ? "Connect" : "Manage", primary: connection == nil) { selected = service }
            }.padding(16).frame(maxWidth: .infinity, alignment: .leading)
             .background(Theme.canvas).overlay(Rectangle().strokeBorder(Theme.rule))
        }
        if !error.isEmpty { Text(error) }
        Color.clear.frame(height: 0).onAppear {
            for service in Service.allCases {
                do { connections[service] = try store.read(service) } catch { self.error = error.localizedDescription }
            }
        }.sheet(item: $selected) { service in
            ConnectorSheet(service: service, connection: connections[service]) { value in
                if let value { try store.save(value, for: service) } else { try store.forget(service) }
                connections[service] = value
            }
        }
    }
}

private struct ConnectorMark: View {
    let service: Service
    private static func load(_ name: String) -> String {
        Bundle.main.url(forResource: name, withExtension: "txt").flatMap { try? String(contentsOf: $0, encoding: .utf8) }?
            .trimmingCharacters(in: .newlines) ?? ""
    }
    static let slack = load("salck"), github = load("github")
    var body: some View {
        Text(verbatim: service == .slack ? Self.slack : Self.github)
            .font(.custom("GeistMono-Regular", size: 3.7)).lineSpacing(0).fixedSize()
            .foregroundColor(Theme.ink).accessibilityHidden(true)
    }
}

private struct ConnectorSheet: View {
    let service: Service
    @State var connection: Connection?
    let persist: (Connection?) throws -> Void
    @Environment(\.dismiss) var dismiss
    @State private var token = ""
    @State private var scope = ""
    @State private var repositories = ""
    @State private var channel = ""
    @State private var message = ""
    @State private var busy = false
    @State private var notice = ""
    @State private var problem = ""
    @State private var confirmForget = false
    private let api = ConnectorAPI()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text(service.rawValue).font(Theme.display(24))
                Spacer()
                actionButton("Done") { dismiss() }.disabled(busy)
            }.padding(18)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    if let c = connection {
                        LabelText(text: "Connected as")
                        Text("\(c.user) · \(c.scope)").font(Theme.display(18))
                        if service == .slack {
                            TextField("Channel ID (e.g. C0123456789)", text: $channel)
                            TextField("Message", text: $message, axis: .vertical).lineLimit(2...5)
                            actionButton("Send as " + c.user, primary: true) {
                                run {
                                    try await api.send(message, channel: channel.trimmingCharacters(in: .whitespacesAndNewlines), using: c)
                                    message = ""; notice = "Message sent."
                                }
                            }.disabled(channel.isEmpty || message.isEmpty)
                        } else {
                            RepositoryTools(connection: c, busy: $busy).id(c.scopeID)
                        }
                        Divider()
                        actionButton("Disconnect") { confirmForget = true }
                    } else {
                        if service == .slack {
                            Text("Create the HERMES Slack app in the workspace you want. Under OAuth & Permissions, install it and copy its User OAuth Token.")
                            Link("Set up in Slack", destination: slackSetupURL)
                            TextField("Workspace ID (T… in your Slack web URL)", text: $scope)
                        } else {
                            Text("Choose your organization as the token’s resource owner and select only the repositories HERMES should access. Grant Contents and Pull requests read and write access.")
                            TextField("Organization login", text: $scope)
                            Link("Create a fine-grained token on GitHub", destination: githubSetupURL)
                            TextField("Repository names, separated by commas", text: $repositories)
                        }
                        SecureField(service == .slack ? "User OAuth token" : "Fine-grained personal access token", text: $token)
                        Text("Stored in this Mac’s Keychain.").foregroundColor(Theme.muted)
                        actionButton("Verify and connect", primary: true) {
                            run {
                                let credential = token.trimmingCharacters(in: .whitespacesAndNewlines)
                                let target = scope.trimmingCharacters(in: .whitespacesAndNewlines)
                                let c: Connection
                                if service == .slack { c = try await api.connectSlack(token: credential, workspace: target) }
                                else { c = try await api.connectGitHub(token: credential, org: target, repos: entries(repositories)) }
                                try persist(c)
                                connection = c; token = ""; notice = ""
                            }
                        }.disabled(token.isEmpty || scope.isEmpty || (service == .github && entries(repositories).isEmpty))
                    }
                    if busy { ProgressView().controlSize(.small) }
                    if !notice.isEmpty { Text(notice) }
                    if !problem.isEmpty { Text(problem).textSelection(.enabled) }
                }.font(.custom("Inter-Regular", size: 14)).frame(maxWidth: .infinity, alignment: .leading)
                 .padding(18).disabled(busy)
            }
        }.frame(minWidth: 420, idealWidth: 500, minHeight: 300, idealHeight: 470)
         .background(Theme.canvas).foregroundColor(Theme.ink).tint(Theme.ink)
         .interactiveDismissDisabled(busy)
         .alert("Disconnect \(service.rawValue) from HERMES?", isPresented: $confirmForget) {
            Button("Cancel", role: .cancel) {}
            Button("Disconnect", role: .destructive) {
                do { try persist(nil); dismiss() } catch { problem = error.localizedDescription }
            }
         } message: { Text("This removes the credential from this Mac. You can separately revoke the token in \(service.rawValue).") }
    }

    private var slackSetupURL: URL {
        var u = URLComponents(string: "https://api.slack.com/apps")!
        let manifest = #"{"display_information":{"name":"HERMES"},"oauth_config":{"scopes":{"user":["chat:write"]}}}"#
        u.queryItems = [.init(name: "new_app", value: "1"), .init(name: "manifest_json", value: manifest)]
        return u.url!
    }

    private var githubSetupURL: URL {
        var u = URLComponents(string: "https://github.com/settings/personal-access-tokens/new")!
        u.queryItems = [.init(name: "name", value: "HERMES"), .init(name: "contents", value: "write"),
                        .init(name: "pull_requests", value: "write"), .init(name: "target_name", value: scope)]
        return u.url!
    }

    private func run(_ work: @escaping @MainActor () async throws -> Void) {
        guard !busy else { return }
        busy = true; problem = ""; notice = ""
        Task { @MainActor in
            defer { busy = false }
            do { try await work() } catch { problem = error.localizedDescription }
        }
    }
}

private struct RepositoryTools: View {
    let connection: Connection
    @Binding var busy: Bool
    @State private var repo = ""
    @State private var title = ""
    @State private var head = ""
    @State private var base = ""
    @State private var reviewers = ""
    @State private var branches: [BranchInfo] = []
    @State private var pulls: [Pull] = []
    @State private var number = 0
    @State private var method = "merge"
    @State private var result = ""
    @State private var confirmMerge = false
    private let api = ConnectorAPI()
    private var selected: Pull? { pulls.first { $0.number == number } }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Picker("Repository", selection: $repo) {
                Text("Select repository").tag("")
                ForEach(connection.repos, id: \.self) { Text($0).tag($0) }
            }.onChange(of: repo) { _ in
                branches = []; pulls = []; number = 0; head = ""; base = ""; title = ""; reviewers = ""; result = ""
                if !repo.isEmpty { refresh() }
            }
            if !repo.isEmpty {
                actionButton("Refresh") { refresh() }
                LabelText(text: "Recent branches")
                if branches.isEmpty { Text("No branches loaded.") }
                ForEach(branches.prefix(8), id: \.name) { branch in
                    HStack {
                        Text(verbatim: branch.name).lineLimit(1).truncationMode(.middle)
                        Spacer()
                        Text(String(branch.target.committedDate.prefix(10))).foregroundColor(Theme.muted)
                    }
                }
                Divider()
                LabelText(text: "New pull request")
                branchPicker("From", selection: $head)
                branchPicker("Into", selection: $base)
                TextField("Pull request title", text: $title)
                actionButton("Open pull request") {
                    run {
                        let created = try await api.createPull(repo: repo, head: head, base: base, title: title, connection: connection)
                        pulls.insert(created, at: 0); number = created.number; title = ""
                        result = "Opened PR #\(created.number)."
                    }
                }.disabled(title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || head.isEmpty || base.isEmpty || head == base)
                Divider()
                LabelText(text: "Open pull requests")
                Picker("Pull request", selection: $number) {
                    Text("Select pull request").tag(0)
                    ForEach(pulls) { Text("#\($0.number) · \($0.title)").tag($0.number) }
                }
                if let p = selected {
                    TextField("Reviewer logins, separated by commas", text: $reviewers)
                    actionButton("Request review") {
                        run {
                            try await api.requestReview(repo: repo, pull: p, users: entries(reviewers), connection: connection)
                            reviewers = ""; result = "Review requested for #\(p.number)."
                        }
                    }.disabled(entries(reviewers).isEmpty)
                    Picker("Merge method", selection: $method) {
                        Text("Merge commit").tag("merge"); Text("Squash").tag("squash"); Text("Rebase").tag("rebase")
                    }
                    actionButton("Merge pull request") { confirmMerge = true }
                }
                if !result.isEmpty { Text(result).textSelection(.enabled) }
            }
        }.alert("Merge #\(number) in \(connection.scope)/\(repo)?", isPresented: $confirmMerge) {
            Button("Cancel", role: .cancel) {}
            Button("Merge", role: .destructive) {
                guard let p = selected else { return }
                run {
                    try await api.merge(repo: repo, pull: p, method: method, connection: connection)
                    pulls.removeAll { $0.number == p.number }; number = 0
                    result = "Merged PR #\(p.number)."
                }
            }
        } message: { Text(selected?.title ?? "") }
    }

    private func branchPicker(_ label: String, selection: Binding<String>) -> some View {
        Picker(label, selection: selection) {
            Text("Select branch").tag("")
            ForEach(branches, id: \.name) { Text(verbatim: $0.name).tag($0.name) }
        }
    }

    private func refresh() {
        run {
            async let refs = api.recentBranches(repo, connection)
            async let prs = api.openPulls(repo, connection)
            let (newBranches, newPulls) = try await (refs, prs)
            branches = newBranches; pulls = newPulls
            if selected == nil { number = 0 }
        }
    }

    private func run(_ work: @escaping @MainActor () async throws -> Void) {
        guard !busy else { return }
        busy = true; result = ""
        Task { @MainActor in
            defer { busy = false }
            do { try await work() } catch { result = error.localizedDescription }
        }
    }
}
