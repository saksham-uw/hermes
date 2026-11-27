import SwiftUI

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
                    WorkspaceCanvas(showGrid: showGrid).frame(height: 104)
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
    @Published var reviewRevisions: [Int: Int] = [:]
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

private extension RepositoryState {
    var productionSummary: String {
        if !live.isEmpty { return "Runtime · " + live }
        if !imageError.isEmpty { return "Prod · " + imageError }
        guard let result = published else { return "Reading production tag…" }
        let tags = Set(result.images.compactMap { $0.split(separator: ":").last.map(String.init) }).sorted()
        guard let tag = tags.first(where: { $0.hasPrefix("prod-") }) ?? tags.first else { return "No published tag" }
        return "Prod build · " + tag
    }
}

private struct RepoCard: View {
    let repo: String
    let owner: String
    @ObservedObject var state: RepositoryState
    let open: () -> Void
    @State private var hover = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        ZStack {
            // All card whitespace is an open target. Text remains available to the selection responder.
            Button(action: open) { Rectangle().fill(Color.clear).contentShape(Rectangle()) }
                .buttonStyle(.plain).accessibilityLabel("Open " + repo)
            VStack(alignment: .leading, spacing: 17) {
                HStack(spacing: 12) {
                    RepositoryGlyph(spread: hover && !reduceMotion ? 1 : 0)
                        .frame(width: 34, height: 34).background(Theme.canvas)
                        .allowsHitTesting(false)
                    VStack(alignment: .leading, spacing: 3) {
                        CopyText(owner, size: 11, face: "GeistMono-Regular", muted: true, activate: open)
                        CopyText(repo, size: 22, face: "Archivo-SemiBold", activate: open)
                    }
                    Spacer(minLength: 2)
                    Text("↗").font(.custom("GeistMono-Regular", size: 19))
                        .foregroundColor(hover ? Theme.ink : Theme.muted).allowsHitTesting(false)
                }
                HStack(spacing: 20) {
                    HStack(spacing: 6) {
                        WorkflowMark.branch.icon().allowsHitTesting(false)
                        stat(state.activity.map { $0.recent().count }, singular: "branch · 48h", plural: "branches · 48h")
                    }
                    HStack(spacing: 6) {
                        WorkflowMark.pull.icon().allowsHitTesting(false)
                        stat(state.activity?.pulls.count, singular: "open PR", plural: "open PRs")
                    }
                }
                Hairline().allowsHitTesting(false)
                CopyText(state.error.isEmpty ? state.productionSummary : state.error,
                         size: 11, face: "GeistMono-Regular", muted: true, activate: open)
            }.padding(18)
        }
        .background(hover ? Color(white: 0.235) : Theme.surface)
        .overlay(Rectangle().strokeBorder(hover ? Theme.muted : Theme.rule).allowsHitTesting(false))
        .shadow(color: .black.opacity(hover ? 0.23 : 0), radius: 6, y: 4)
        .offset(y: hover && !reduceMotion ? -3 : 0)
        .animation(reduceMotion ? nil : .easeOut(duration: 0.16), value: hover)
        .background(PointerRegion(cursor: HermesPointer.hand, onHoverChange: { hover = $0 }))
        .contentShape(Rectangle())
    }

    private func stat(_ count: Int?, singular: String, plural: String) -> some View {
        CopyText((count.map(String.init) ?? "—") + " " + (count == 1 ? singular : plural), size: 12, face: "GeistMono-Regular", activate: open)
    }
}

private enum RepoSection: String, CaseIterable { case branches = "Branches", production = "Production" }

private struct RepoDetail: View {
    let repo: String
    let github: Connection
    let slack: Connection?
    @ObservedObject var state: RepositoryState
    let back: () -> Void
    let settings: (SettingsSection) -> Void
    @State private var revision = 0
    @State private var ahead: Int?
    @State private var section = RepoSection.branches
    @State private var expanded: String?
    @State private var showImages = false
    @State private var wideRows = false
    @State private var mergeFeedback = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 17) {
            HStack {
                actionButton("‹ Repositories", action: back).keyboardShortcut(.cancelAction)
                Spacer()
                actionButton("Refresh") { mergeFeedback = ""; revision += 1 }
            }.disabled(state.busy)
            VStack(alignment: .leading, spacing: 3) {
                CopyText(github.scopeID, size: 12, face: "GeistMono-Regular", muted: true)
                CopyText(repo, size: 30, face: "Archivo-SemiBold")
            }
            HStack(spacing: 4) {
                ForEach(RepoSection.allCases, id: \.self) { item in
                    Button { mergeFeedback = ""; section = item } label: { LabelText(text: item.rawValue, bold: true) }
                        .buttonStyle(TabStyle(selected: section == item))
                        .accessibilityAddTraits(section == item ? .isSelected : [])
                }
            }.disabled(state.busy)
            if !mergeFeedback.isEmpty { Text(mergeFeedback).textSelection(.enabled) }
            if !state.error.isEmpty { Text(state.error) }
            if let data = state.activity {
                switch section {
                case .branches:
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(data.mainQueue(fullName: github.scopeID + "/" + repo)) { item in
                            switch item {
                            case let .openHeader(count):
                                queueHeading("Open PRs", count: count, caption: "into main")
                                if count > 0 { CheckPatternKey().padding(.bottom, 9) }
                            case let .branchHeader(count):
                                queueHeading("Branches without a PR", count: count, caption: "48h · into main")
                                    .padding(.top, 14)
                            case let .work(head, pull, committed):
                                row(head, pull: pull, committed: committed, data: data)
                            }
                        }
                    }
                case .production:
                    CopyText(state.productionSummary, size: 14, face: "GeistMono-Regular")
                    if data.branches.contains(where: { $0.name == "main" }) && data.branches.contains(where: { $0.name == "prod" }) {
                        HStack {
                            WorkflowMark.branch.icon(20)
                            Text("main").font(Theme.display(18))
                            Rectangle().fill(Theme.muted).frame(width: 24, height: 1)
                            WorkflowMark.merge.icon(20)
                            Text("prod").font(Theme.display(18))
                            Spacer()
                            if let ahead { Text(ahead == 0 ? "Up to date" : "\(ahead) ahead").foregroundColor(Theme.muted) }
                        }
                        if let pull = data.pull(head: "main", base: "prod", fullName: github.scopeID + "/" + repo) {
                            CheckPatternKey()
                            statusLine(pull, expanded: false)
                        }
                        lane("main", "prod", data)
                    } else { Text("Promotion requires main and prod branches.") }
                    Hairline()
                    actionButton(showImages ? "Hide image details" : "Image details") { showImages.toggle() }
                    if showImages {
                        if state.live.isEmpty { actionButton("Configure live runtime") { settings(.workflow) } }
                        if let result = state.published {
                            ForEach(result.images, id: \.self) { CopyText($0, size: 12, face: "GeistMono-Regular") }
                            if !result.note.isEmpty { Text(result.note) }
                            if let url = result.run { actionButton("View build ↗") { NSWorkspace.shared.open(url) } }
                        }
                    }
                }
            } else if state.loading { LoadingMark(title: "Loading repository…") }
        }.background {
            GeometryReader { proxy in
                Color.clear.onAppear { wideRows = proxy.size.width >= 690 }
                    .onChange(of: proxy.size.width >= 690) { wideRows = $0 }
            }
        }.task(id: revision) {
            async let distance = try? state.api.promotionDistance(repo: repo, connection: github)
            await state.load(repo, github)
            let result = await distance
            if !Task.isCancelled { ahead = result }
        }
    }

    private func queueHeading(_ title: String, count: Int, caption: String) -> some View {
        HStack(spacing: 9) {
            Text(title).font(Theme.display(19))
            Text(String(count)).font(.custom("GeistMono-Medium", size: 11))
                .padding(.horizontal, 6).padding(.vertical, 2)
                .overlay(Rectangle().strokeBorder(Theme.rule))
            Spacer()
            Text(caption).font(.custom("GeistMono-Regular", size: 11)).foregroundColor(Theme.muted)
        }.padding(.vertical, 8).accessibilityElement(children: .combine)
    }

    private func row(_ name: String, pull: Pull?, committed: String?, data: RepoActivity) -> some View {
        let active = expanded == name
        let toggle = { if !state.busy { expanded = active ? nil : name } }
        return VStack(alignment: .leading, spacing: 0) {
            Hairline()
            ZStack {
                Button(action: toggle) { Color.clear.contentShape(Rectangle()) }
                    .buttonStyle(.plain).accessibilityLabel((active ? "Collapse " : "Expand ") + (pull?.title ?? name))
                HStack(spacing: 10) {
                    (pull == nil ? WorkflowMark.branch : .pull).icon(18)
                        .frame(width: 30, height: 30)
                        .foregroundColor(pull == nil ? Theme.muted : Theme.canvas)
                        .background(pull == nil ? Theme.canvas : Theme.ink)
                        .overlay(Rectangle().strokeBorder(Theme.rule)).allowsHitTesting(false)
                    let layout = wideRows && !active ? AnyLayout(HStackLayout(spacing: 10)) :
                        AnyLayout(VStackLayout(alignment: .leading, spacing: 7))
                    layout {
                        VStack(alignment: .leading, spacing: 3) {
                            CopyText(pull?.title ?? name, size: pull == nil ? 13 : 15,
                                     face: pull == nil ? "GeistMono-Regular" : "Archivo-SemiBold", activate: toggle)
                            if let pull {
                                CopyText("#\(pull.number) · " + name, size: 11, face: "GeistMono-Regular", muted: true, activate: toggle)
                            }
                        }.frame(maxWidth: .infinity, alignment: .leading)
                        if let pull { statusLine(pull, expanded: active) }
                    }
                    if let committed, let date = ISO8601DateFormatter().date(from: committed) {
                        CopyText(abs(date.timeIntervalSinceNow) < 60 ? "Just now" : date.formatted(.relative(presentation: .numeric)),
                                 size: 11, muted: true, activate: toggle)
                    }
                    Path { p in
                        p.move(to: CGPoint(x: 2, y: 1)); p.addLine(to: CGPoint(x: 7, y: 6)); p.addLine(to: CGPoint(x: 2, y: 11))
                    }.stroke(Theme.muted, lineWidth: 1.4).frame(width: 10, height: 12)
                     .rotationEffect(.degrees(active ? 90 : 0)).allowsHitTesting(false)
                }.padding(.horizontal, 12).padding(.vertical, 9)
            }.modifier(ControlHover()).disabled(state.busy)
            if active { lane(name, "main", data).id(name) }
        }.background(active || pull != nil ? Theme.surface : Color.clear)
    }

    private func statusLine(_ pull: Pull, expanded: Bool) -> some View {
        PullStatusLine(repo: repo, github: github, pull: pull, api: state.api, expanded: expanded) { action in
            merge(pull, action: action)
        }.disabled(state.busy)
            .id("\(pull.number)/\(pull.head.sha)/\(revision)/\(state.reviewRevisions[pull.number, default: 0])")
    }

    private func merge(_ pull: Pull, action: MergeAction) {
        guard !state.busy, let base = pull.base?.ref else { return }
        state.busy = true
        mergeFeedback = "Merging #\(pull.number) into \(base)…"
        Task { @MainActor in
            defer {
                state.busy = false
                revision += 1
            }
            do {
                if action == .ready {
                    async let readiness = state.api.pullReadiness(repo: repo, pull: pull, connection: github)
                    async let checks = state.api.checks(repo: repo, pull: pull, connection: github)
                    let (current, results) = try await (readiness, checks)
                    guard current.readyForPrimaryMerge(checks: results) else {
                        throw ConnectorError("PR status changed. Check the latest review and CI results.")
                    }
                }
                try await state.api.merge(repo: repo, pull: pull, method: "merge", connection: github)
                state.remember(nil, replacing: pull.number)
                if base == "prod" { ahead = nil }
                mergeFeedback = "Merged #\(pull.number) into \(base)."
            } catch {
                mergeFeedback = error.localizedDescription
            }
        }
    }

    private func lane(_ head: String, _ base: String, _ data: RepoActivity) -> some View {
        BranchWorkflow(repo: repo, head: head, base: base, github: github, slack: slack, state: state,
                       pull: data.pull(head: head, base: base, fullName: github.scopeID + "/" + repo),
                       hasBase: data.branches.contains { $0.name == base } && (base != "prod" || ahead != 0),
                       settings: { settings(.workflow) })
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
    let hasBase: Bool
    let settings: () -> Void
    @State private var feedback = ""
    @State private var working = false
    @State private var editing = false
    @State private var reviewerOverride: String?
    @State private var recipientOverride: String?

    private var reviewers: [String] {
        WorkflowKeys.reviewers(reviewerOverride ?? UserDefaults.standard.string(forKey: WorkflowKeys.reviewer(github)) ?? "")
    }
    private var recipient: String {
        recipientOverride ?? slack.flatMap { UserDefaults.standard.string(forKey: WorkflowKeys.recipient($0)) } ?? ""
    }
    private func url(_ pull: Pull) -> URL { URL(string: "https://github.com/\(github.scopeID)/\(repo)/pull/\(pull.number)")! }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let pull, base == "prod" {
                HStack {
                    CopyText("#\(pull.number)  " + pull.title, size: 15, face: "Archivo-SemiBold")
                    Spacer()
                    actionButton("GitHub ↗") { NSWorkspace.shared.open(url(pull)) }
                }
            }
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 8) { actions }
                VStack(alignment: .leading, spacing: 8) { actions }
            }.disabled(state.busy)
            if working { LoadingMark(title: "Working…") }
            if !feedback.isEmpty { Text(feedback).textSelection(.enabled) }
            if editing {
                VStack(alignment: .leading, spacing: 8) {
                    HermesField("GitHub reviewers", text: Binding(get: { reviewerOverride ?? reviewers.joined(separator: ", ") }, set: { reviewerOverride = $0 }))
                    if slack != nil {
                        HermesField("Slack member ID or DM link", text: Binding(get: { recipient }, set: { recipientOverride = $0 }))
                    }
                    actionButton("Manage saved defaults", action: settings)
                }.disabled(state.busy)
            }
        }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.surface)
    }

    @ViewBuilder private var actions: some View {
        if let pull {
            actionButton("Request review") {
                guard !reviewers.isEmpty else { editing = true; feedback = "Choose a GitHub reviewer."; return }
                let users = reviewers
                perform {
                    try await state.api.requestReview(repo: repo, pull: pull, users: users, connection: github)
                    state.reviewRevisions[pull.number, default: 0] += 1
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
            if base == "main" {
                actionButton("GitHub ↗") { NSWorkspace.shared.open(url(pull)) }
            }
            actionButton("Options") { editing.toggle() }
        } else {
            actionButton(reviewers.isEmpty ? "Open PR" : "Open PR + request review", primary: true) {
                let users = reviewers
                perform {
                    let created = try await state.api.ensurePull(repo: repo, head: head, base: base, connection: github)
                    state.remember(created, replacing: created.number)
                    if !users.isEmpty {
                        do {
                            try await state.api.requestReview(repo: repo, pull: created, users: users, connection: github)
                            state.reviewRevisions[created.number, default: 0] += 1
                        }
                        catch { return "PR #\(created.number) is open. Review request failed. " + error.localizedDescription }
                    }
                    return users.isEmpty ? "PR #\(created.number) is open." : "PR #\(created.number) is open. Requested " + users.joined(separator: ", ") + "."
                }
            }.disabled(!hasBase)
            actionButton("Recipients") { editing.toggle() }
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
