import SwiftUI

func actionButton(_ title: String, primary: Bool = false, action: @escaping () -> Void) -> some View {
    Button(action: action) { LabelText(text: title, bold: true) }.buttonStyle(ActionStyle(primary: primary))
}

struct ConnectorsPage: View {
    @State private var connections: [Service: Connection] = [:]
    @State private var selected: Service?
    @State private var error = ""
    private let store = ConnectionStore()

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            ForEach(Service.allCases) { service in
                let connection = connections[service]
                VStack(alignment: .leading, spacing: 12) {
                    HStack(spacing: 14) {
                        ConnectorMark(service: service)
                        VStack(alignment: .leading, spacing: 5) {
                            Text(service.rawValue).font(Theme.display(20))
                            Text(connection.map { $0.scope.isEmpty ? "\($0.user) · Choose repositories" : "\($0.user) · \($0.scope)" } ?? "Disconnected")
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
        }.onAppear {
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
    @State private var editingRepositories = false
    @State private var workTask: Task<Void, Never>?
    @State private var busy = false
    @State private var problem = ""
    @State private var confirmForget = false
    private let api = ConnectorAPI()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text(service.rawValue).font(Theme.display(24))
                Spacer()
                actionButton("Done") { dismiss() }
            }.padding(18)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    if let c = connection {
                        LabelText(text: "Connected as")
                        Text(c.scope.isEmpty ? c.user : "\(c.user) · \(c.scope)").font(Theme.display(18))
                        if service == .github {
                            if c.grant != nil && (editingRepositories || c.repos.isEmpty) {
                                GitHubRepositorySelection(connection: c) { owner, repos in
                                    var latest = try latestConnection(c)
                                    latest.scope = owner
                                    latest.scopeID = owner
                                    latest.repos = repos
                                    try save(latest)
                                }.id(c.grant?.id)
                                if !c.repos.isEmpty {
                                    actionButton("Cancel selection") { editingRepositories = false }
                                }
                            } else {
                                if c.grant != nil {
                                    actionButton("Choose repositories") { editingRepositories = true }
                                }
                                ForEach(c.repos, id: \.self) { Text(verbatim: c.scopeID + "/" + $0) }
                            }
                        }
                        Divider()
                        actionButton("Reconnect account") { signIn() }
                        actionButton("Disconnect") { confirmForget = true }
                    } else {
                        Text("Approve HERMES on " + service.rawValue + " to continue.")
                        actionButton("Authorize with " + service.rawValue, primary: true) { signIn() }
                    }
                    if busy { ProgressView().controlSize(.small) }
                    if !problem.isEmpty { Text(problem).textSelection(.enabled) }
                }.font(.custom("Inter-Regular", size: 14)).frame(maxWidth: .infinity, alignment: .leading)
                 .padding(18).disabled(busy)
            }
        }.frame(minWidth: 420, idealWidth: 500, minHeight: 300, idealHeight: 470)
         .background(Theme.canvas).foregroundColor(Theme.ink).tint(Theme.ink)
         .onDisappear { workTask?.cancel() }
         .onAppear {
            if connection == nil { signIn() }
         }
         .alert("Disconnect \(service.rawValue) from HERMES?", isPresented: $confirmForget) {
            Button("Cancel", role: .cancel) {}
            Button("Disconnect", role: .destructive) {
                do { try persist(nil); dismiss() } catch { problem = error.localizedDescription }
            }
         } message: { Text("This removes the credential from this Mac. You can separately revoke the token in \(service.rawValue).") }
    }

    private func save(_ c: Connection) throws {
        try persist(c)
        connection = c; editingRepositories = false
    }

    private func latestConnection(_ c: Connection) throws -> Connection {
        guard let latest = try ConnectionStore().read(service), latest.grant?.id == c.grant?.id else {
            throw ConnectorError("Connection changed. Reopen Connectors.")
        }
        return latest
    }

    private func signIn() {
        run {
            let grant = try await BrowserAuthorization().authorize(service)
            if service == .slack {
                guard let team = grant.workspace else { throw ConnectorError("Slack did not return a workspace.") }
                var c = try await api.connectSlack(token: grant.token, workspace: team)
                c.grant = grant.grant
                try Task.checkCancellation()
                try save(c)
            } else {
                let user = try await api.githubUser(grant.token)
                let previous = connection.flatMap { $0.user == user ? $0 : nil }
                let c = Connection(user: user, scope: previous?.scope ?? "", scopeID: previous?.scopeID ?? "",
                                   repos: previous?.repos ?? [], token: grant.token, grant: grant.grant, installURL: grant.install)
                try Task.checkCancellation()
                try save(c)
                editingRepositories = true
            }
        }
    }

    private func run(_ work: @escaping @MainActor () async throws -> Void) {
        guard !busy else { return }
        busy = true; problem = ""
        workTask = Task { @MainActor in
            defer { busy = false }
            do { try await work() } catch { problem = error.localizedDescription }
        }
    }
}

private struct GitHubRepositorySelection: View {
    let connection: Connection
    let save: (String, [String]) throws -> Void
    @State private var installations: [ConnectorAPI.Installation] = []
    @State private var installationID: Int?
    @State private var repositories: [String] = []
    @State private var chosen: Set<String> = []
    @State private var reload = 0
    @State private var loadingAccounts = true
    @State private var loadedInstallation: Int?
    @State private var awaitingGitHub = false
    @State private var accountError = ""
    @State private var repoError = ""
    private let api = ConnectorAPI()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            LabelText(text: "Repository access")
            if loadingAccounts {
                ProgressView("Loading GitHub accounts…").controlSize(.small)
            } else if !accountError.isEmpty {
                Text(accountError).textSelection(.enabled)
            } else if installations.isEmpty {
                Text("To see repositories here, install HERMES on your organization or personal account and choose the repositories on GitHub. Your organization may require owner approval.")
            } else {
                Picker("Organization or account", selection: $installationID) {
                    ForEach(installations) { item in
                        Text(item.account.login).tag(Optional(item.id))
                    }
                }
                if loadedInstallation != installationID || installationID == nil {
                    ProgressView("Loading repositories…").controlSize(.small)
                } else if !repoError.isEmpty {
                    Text(repoError).textSelection(.enabled)
                } else if repositories.isEmpty {
                    Text("GitHub returned no accessible repositories for this account. Check the repository access granted to HERMES on GitHub.")
                } else {
                    ForEach(repositories, id: \.self) { repo in
                        Toggle(repo, isOn: Binding(get: { chosen.contains(repo) }, set: { enabled in
                            if enabled { chosen.insert(repo) } else { chosen.remove(repo) }
                        })).toggleStyle(.checkbox)
                    }
                    actionButton("Save repositories", primary: true) {
                        guard let owner = installations.first(where: { $0.id == installationID }) else { return }
                        do { try save(owner.account.login, chosen.sorted()) }
                        catch { repoError = error.localizedDescription }
                    }.disabled(chosen.isEmpty)
                }
            }
            actionButton("Manage access on GitHub") {
                let url = connection.installURL ?? URL(string: "https://github.com/settings/installations")!
                awaitingGitHub = NSWorkspace.shared.open(url)
            }
            actionButton("Reload accounts") { reload += 1 }
        }
        .task(id: reload) { await loadAccounts() }
        .task(id: installationID) { await loadRepositories() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            if awaitingGitHub { awaitingGitHub = false; reload += 1 }
        }
    }

    @MainActor private func loadAccounts() async {
        let previousID = installationID
        loadingAccounts = true
        accountError = ""
        installationID = nil
        do {
            let token = try await OAuthCredentials.shared.token(.github, connection)
            let accounts = try await api.installations(token)
            try Task.checkCancellation()
            installations = accounts
            installationID = accounts.first { $0.id == previousID }?.id
                ?? accounts.first { $0.account.login == connection.scopeID }?.id
                ?? accounts.first?.id
        } catch {
            guard !Task.isCancelled else { return }
            accountError = error.localizedDescription
        }
        loadingAccounts = false
    }

    @MainActor private func loadRepositories() async {
        repositories = []; chosen = []; repoError = ""
        guard let item = installations.first(where: { $0.id == installationID }) else { return }
        loadedInstallation = nil
        do {
            let token = try await OAuthCredentials.shared.token(.github, connection)
            let repos = try await api.repositories(item, token: token)
            try Task.checkCancellation()
            repositories = repos
            if connection.scopeID == item.account.login { chosen = Set(connection.repos).intersection(repos) }
        } catch {
            guard !Task.isCancelled else { return }
            repoError = error.localizedDescription
        }
        loadedInstallation = item.id
    }
}
