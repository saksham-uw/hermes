import SwiftUI

private let phosphor = Color(red: 0.62, green: 0.86, blue: 0.81)
private let slate = Color(red: 0.18, green: 0.21, blue: 0.23)

struct RepositoryWorkspace: View {
    let showGrid: Bool
    let settings: (SettingsSection) -> Void
    @State private var github: Connection?
    @State private var slack: Connection?
    @State private var selected: String?
    @State private var problem = ""
    @State private var revision = 0
    @State private var items: [String: RepositoryState] = [:]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                if let github, let selected, let state = items[selected] {
                    RepoDetail(repo: selected, github: github, slack: slack, state: state,
                               back: { self.selected = nil }, settings: settings).id(selected)
                } else {
                    WorkspaceCanvas(showGrid: showGrid).frame(height: 88)
                    HStack {
                        Text("Repositories").font(Theme.display(24))
                        Spacer()
                        if let github, !github.repos.isEmpty {
                            actionButton("Refresh") { revision += 1 }
                        }
                    }
                    if let github, !github.repos.isEmpty {
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: 310), spacing: 12)], spacing: 12) {
                            ForEach(github.repos, id: \.self) { repo in
                                if let state = items[repo] {
                                    RepoCard(repo: repo, owner: github.scopeID, state: state) { selected = repo }
                                        .task(id: revision) { await state.load(repo, github, revision: revision) }
                                }
                            }
                        }
                    } else {
                        Text(github == nil ? "Connect GitHub to add repositories to Workspace." : "Choose repositories for this GitHub connection.")
                        actionButton("Manage connections") { settings(.connectors) }
                    }
                }
                if !problem.isEmpty { Text(problem).textSelection(.enabled) }
            }.padding(22).frame(maxWidth: .infinity, alignment: .leading)
        }.frame(maxWidth: .infinity, maxHeight: .infinity).onAppear {
            do {
                github = try ConnectionStore().read(.github)
                if let github {
                    for repo in github.repos where items[repo] == nil { items[repo] = RepositoryState() }
                }
                slack = try ConnectionStore().read(.slack)
            } catch { problem = error.localizedDescription }
        }
    }
}

@MainActor private final class RepositoryState: ObservableObject {
    @Published var activity: RepoActivity?
    @Published var error = ""
    @Published var published: PublishedImages?
    @Published var imageError = ""
    @Published var live = ""
    @Published var loading = false
    @Published var busy = false
    let api = ConnectorAPI()
    private var generation = UUID()
    private var lastLoaded: Date?
    private var lastRevision: Int?
    private var actionRevision = 0

    func remember(_ value: Pull?, replacing number: Int) {
        guard let previous = activity else { return }
        actionRevision += 1
        var pulls = previous.pulls.filter { $0.number != number }
        if let value { pulls.insert(value, at: 0) }
        activity = RepoActivity(branches: previous.branches, pulls: pulls)
    }

    func load(_ repo: String, _ c: Connection, revision: Int? = nil) async {
        if let revision, lastRevision == revision, let lastLoaded, Date().timeIntervalSince(lastLoaded) < 90 { return }
        let startingRevision = actionRevision
        let current = UUID()
        generation = current
        loading = true
        defer { if generation == current { loading = false } }
        async let registry: Void = loadImages(repo, c, current)
        async let runtime: Void = loadRuntime(repo, c, current)
        do {
            let data = try await api.activity(repo, c)
            try Task.checkCancellation()
            if generation == current && startingRevision == actionRevision { activity = data; error = "" }
        } catch {
            if !Task.isCancelled && generation == current { self.error = error.localizedDescription }
        }
        _ = await (registry, runtime)
        if !Task.isCancelled && generation == current {
            lastLoaded = Date()
            if let revision { lastRevision = revision }
        }
    }

    private func loadImages(_ repo: String, _ c: Connection, _ current: UUID) async {
        do {
            let result = try await api.publishedImages(repo: repo, connection: c)
            if !Task.isCancelled && generation == current { published = result; imageError = "" }
        } catch {
            if !Task.isCancelled && generation == current {
                imageError = error.localizedDescription.contains("(403)") ? "Actions read access required" : error.localizedDescription
            }
        }
    }

    private func loadRuntime(_ repo: String, _ c: Connection, _ current: UUID) async {
        let context = (UserDefaults.standard.string(forKey: "workflow.docker." + c.scopeID) ?? "").trimmingCharacters(in: .whitespaces)
        guard !context.isEmpty else { live = ""; return }
        do {
            let tags = try await ProductionRuntime.runningImages(context: context, repo: repo)
            if !Task.isCancelled && generation == current { live = tags.isEmpty ? "No matching containers" : tags.joined(separator: ", ") }
        } catch {
            if !Task.isCancelled && generation == current { live = error.localizedDescription }
        }
    }
}

private struct RepoCard: View {
    let repo: String
    let owner: String
    @ObservedObject var state: RepositoryState
    let open: () -> Void
    var body: some View {
        Button(action: open) {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Image(systemName: "point.3.connected.trianglepath.dotted").foregroundColor(phosphor)
                    LabelText(text: owner)
                    Spacer()
                    Image(systemName: "arrow.up.right")
                }.foregroundColor(Theme.muted)
                Text(verbatim: repo).font(Theme.display(24)).lineLimit(1).minimumScaleFactor(0.7)
                Divider()
                HStack(spacing: 22) {
                    metric("Branches · 48h", state.activity.map { $0.recent().count })
                    metric("Open PRs", state.activity?.pulls.count)
                }
                if !state.error.isEmpty { Text(state.error).font(.caption).lineLimit(2) }
                imageSummary.lineLimit(2).font(.custom("GeistMono-Regular", size: 11)).foregroundColor(phosphor)
            }.padding(16).frame(maxWidth: .infinity, minHeight: 172, alignment: .topLeading)
                .background(slate).overlay(Rectangle().strokeBorder(Theme.rule))
                .overlay(alignment: .topLeading) { Rectangle().fill(phosphor).frame(width: 42, height: 2) }
                .contentShape(Rectangle())
        }.buttonStyle(.plain)
    }

    private var imageSummary: Text {
        if !state.live.isEmpty { return Text("Runtime · " + state.live) }
        if !state.imageError.isEmpty { return Text("Prod · " + state.imageError) }
        if let result = state.published {
            let tags = Set(result.images.compactMap { $0.split(separator: ":").last.map(String.init) })
            let pinned = tags.filter { $0.hasPrefix("prod-") }.sorted() + tags.filter { $0 != "latest" }.sorted()
            return Text("Prod build · " + (tags.isEmpty ? "No tag reported" : (pinned.first ?? "latest")))
        }
        return Text("Loading production tag…")
    }

    private func metric(_ label: String, _ count: Int?) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(count.map(String.init) ?? "—").font(Theme.display(20))
            Text(label).font(.custom("GeistMono-Regular", size: 11)).foregroundColor(Theme.muted)
        }
    }
}

private struct RepoDetail: View {
    let repo: String
    let github: Connection
    let slack: Connection?
    @ObservedObject var state: RepositoryState
    let back: () -> Void
    let settings: (SettingsSection) -> Void
    @State private var revision = 0
    @State private var ahead: Int?

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                actionButton("‹ Repositories", action: back)
                Spacer()
                actionButton("Refresh") { revision += 1 }
            }.disabled(state.busy)
            Text(verbatim: github.scopeID + " / " + repo).font(Theme.display(25))
            VStack(alignment: .leading, spacing: 6) {
                LabelText(text: "Production images", bold: true).foregroundColor(phosphor)
                if !state.live.isEmpty { Text("Runtime · " + state.live).textSelection(.enabled) }
                else { Button("Configure live runtime") { settings(.workflow) }.buttonStyle(.link) }
                if !state.imageError.isEmpty { Text(state.imageError).textSelection(.enabled) }
                if let images = state.published {
                    ForEach(images.images, id: \.self) { Text(verbatim: $0).font(.system(.body, design: .monospaced)).textSelection(.enabled) }
                    if let url = images.run { Link("View production image build ↗", destination: url) }
                    if !images.note.isEmpty { Text(images.note) }
                }
            }.frame(maxWidth: .infinity, alignment: .leading).padding(14).background(slate)
            if !state.error.isEmpty { Text(state.error).textSelection(.enabled) }
            if let data = state.activity {
                LabelText(text: "Production · main → prod", bold: true)
                if data.branches.contains(where: { $0.name == "main" }) && data.branches.contains(where: { $0.name == "prod" }) {
                    if let ahead {
                        Text(ahead == 0 ? "Production is up to date with main." : "main is \(ahead) commit\(ahead == 1 ? "" : "s") ahead of prod.").foregroundColor(Theme.muted)
                    }
                    lane("main", "prod", data)
                } else { Text("This repository needs main and prod branches for promotion.") }
                LabelText(text: "Branch activity · last 48 hours", bold: true)
                let branches = data.recent().filter { $0.name != "main" && $0.name != "prod" }
                if branches.isEmpty { Text("No feature branches with commits in the last 48 hours.").foregroundColor(Theme.muted) }
                ForEach(branches, id: \.name) { branch in
                    lane(branch.name, "main", data)
                }
                let older = data.pulls.filter { pull in
                    pull.head.repo?.full_name.caseInsensitiveCompare(github.scopeID + "/" + repo) == .orderedSame &&
                    pull.base?.ref == "main" && !branches.contains { $0.name == pull.head.ref }
                }
                if !older.isEmpty {
                    LabelText(text: "Other open PRs into main", bold: true)
                    ForEach(older) { pull in
                        if let head = pull.head.ref { lane(head, "main", data) }
                    }
                }
            } else if state.loading { ProgressView("Loading repository…") }
        }.task(id: revision) {
            async let distance = try? state.api.promotionDistance(repo: repo, connection: github)
            await state.load(repo, github)
            let result = await distance
            if !Task.isCancelled { ahead = result }
        }
    }

    private func lane(_ head: String, _ base: String, _ data: RepoActivity) -> some View {
        BranchWorkflow(repo: repo, head: head, base: base, github: github, slack: slack, state: state,
                       pull: data.pull(head: head, base: base, fullName: github.scopeID + "/" + repo),
                       branch: data.branches.first { $0.name == head }, hasBase: data.branches.contains { $0.name == base } && (base != "prod" || ahead != 0),
                       revision: revision, settings: { settings(.workflow) })
    }
}

private struct BranchWorkflow: View {
    let repo: String
    let head: String
    let base: String
    let github: Connection
    let slack: Connection?
    @ObservedObject var state: RepositoryState
    let pull: Pull?
    let branch: BranchInfo?
    let hasBase: Bool
    let revision: Int
    let settings: () -> Void
    @State private var checks = ""
    @State private var feedback = ""
    @State private var working = false
    @State private var editing = false
    @State private var reviewerOverride: String?
    @State private var recipientOverride: String?
    @State private var confirmMerge = false

    private var reviewers: [String] {
        WorkflowKeys.reviewers(reviewerOverride ?? UserDefaults.standard.string(forKey: WorkflowKeys.reviewer(github)) ?? "")
    }
    private var recipient: String {
        recipientOverride ?? slack.flatMap { UserDefaults.standard.string(forKey: WorkflowKeys.recipient($0)) } ?? ""
    }
    private func url(_ pull: Pull) -> URL { URL(string: "https://github.com/\(github.scopeID)/\(repo)/pull/\(pull.number)")! }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Image(systemName: "arrow.triangle.branch").foregroundColor(phosphor)
                Text(verbatim: head).font(Theme.display(17)).lineLimit(2)
                Text("→ " + base).foregroundColor(Theme.muted)
                Spacer()
                if let branch, let date = ISO8601DateFormatter().date(from: branch.target.committedDate) {
                    Text(date, style: .relative).font(.caption).help(date.formatted())
                }
            }
            if let pull {
                Link("#\(pull.number)  " + pull.title, destination: url(pull))
                Text(checks.isEmpty ? "Loading checks…" : checks).font(.custom("GeistMono-Regular", size: 11)).foregroundColor(Theme.muted)
            }
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 8) { actions }
                VStack(alignment: .leading, spacing: 8) { actions }
            }.disabled(state.busy)
            if working { ProgressView().controlSize(.small) }
            if !feedback.isEmpty { Text(feedback).textSelection(.enabled) }
            if editing {
                TextField("GitHub reviewers", text: Binding(get: { reviewerOverride ?? reviewers.joined(separator: ", ") }, set: { reviewerOverride = $0 }))
                if slack != nil {
                    TextField("Slack member ID or DM link", text: Binding(get: { recipient }, set: { recipientOverride = $0 }))
                }
                Button("Manage saved defaults", action: settings).buttonStyle(.link)
            }
        }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.surface).overlay(Rectangle().strokeBorder(Theme.rule))
        .task(id: "\(pull?.head.sha ?? "")/\(pull?.number ?? 0)/\(revision)") {
            guard let pull else { checks = ""; return }
            do {
                let result = try await state.api.checks(repo: repo, pull: pull, connection: github)
                try Task.checkCancellation()
                checks = result.summary
            } catch {
                if !Task.isCancelled {
                    checks = error.localizedDescription.contains("(403)") ? "Checks unavailable · grant Checks and Commit statuses read access." : error.localizedDescription
                }
            }
        }
        .alert("Merge #\(pull?.number ?? 0) into \(base)?", isPresented: $confirmMerge) {
            Button("Cancel", role: .cancel) {}
            Button("Merge", role: .destructive) {
                guard let pull else { return }
                perform {
                    try await state.api.merge(repo: repo, pull: pull, method: "merge", connection: github)
                    state.remember(nil, replacing: pull.number)
                    return "Merged #\(pull.number) into \(base)."
                }
            }
        } message: { Text(verbatim: github.scopeID + "/" + repo + " · " + head + " → " + base) }
    }

    @ViewBuilder private var actions: some View {
        if let pull {
            actionButton("Request review") {
                guard !reviewers.isEmpty else { editing = true; feedback = "Choose a GitHub reviewer."; return }
                let users = reviewers
                perform {
                    try await state.api.requestReview(repo: repo, pull: pull, users: users, connection: github)
                    return "Requested review from " + users.joined(separator: ", ") + "."
                }
            }.help("Request or re-request review from " + reviewers.joined(separator: ", "))
            if let slack {
                actionButton("Send on Slack") {
                    guard ConnectorAPI.recipientID(recipient) != nil else { editing = true; feedback = "Choose a Slack recipient."; return }
                    let destination = recipient
                    perform {
                        try await state.api.sendPR(url(pull), recipient: destination, connection: slack)
                        return "Sent #\(pull.number) in a direct message."
                    }
                }.help("Send this PR link to " + recipient)
            }
            Menu {
                Button("Change recipients") { editing.toggle() }
                Button("Merge into " + base) { confirmMerge = true }
            } label: { Image(systemName: "ellipsis") }.fixedSize().accessibilityLabel("More pull request actions")
        } else {
            actionButton(reviewers.isEmpty ? "Open PR" : "Open PR + request review", primary: true) {
                let users = reviewers
                perform {
                    let created = try await state.api.ensurePull(repo: repo, head: head, base: base, connection: github)
                    state.remember(created, replacing: created.number)
                    if !users.isEmpty {
                        do { try await state.api.requestReview(repo: repo, pull: created, users: users, connection: github) }
                        catch { return "PR #\(created.number) is open. Review request failed. " + error.localizedDescription }
                    }
                    return users.isEmpty ? "PR #\(created.number) is open." : "PR #\(created.number) is open. Requested " + users.joined(separator: ", ") + "."
                }
            }.disabled(!hasBase)
            Button("Recipients") { editing.toggle() }.buttonStyle(.link)
        }
    }

    private func perform(_ work: @escaping @MainActor () async throws -> String) {
        guard !state.busy else { return }
        state.busy = true; working = true; feedback = ""
        Task { @MainActor in
            defer { state.busy = false; working = false }
            do { feedback = try await work() }
            catch {
                let message = error.localizedDescription
                feedback = message.contains("missing_scope") ? "Reconnect Slack to send to a member ID, or use an existing DM link." : message
            }
        }
    }
}
