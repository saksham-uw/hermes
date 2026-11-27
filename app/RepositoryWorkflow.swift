import Foundation

struct RepoActivity {
    let branches: [BranchInfo]
    let pulls: [Pull]

    func recent(at now: Date = Date()) -> [BranchInfo] {
        let clock = ISO8601DateFormatter()
        let cutoff = now.addingTimeInterval(-48 * 3600)
        return branches.filter {
            guard let date = clock.date(from: $0.target.committedDate) else { return false }
            return date >= cutoff && date <= now
        }
    }

    enum MainQueueItem: Identifiable {
        case openHeader(Int), branchHeader(Int)
        case work(head: String, pull: Pull?, committed: String?)

        var id: String {
            switch self {
            case .openHeader: return "section:open"
            case .branchHeader: return "section:branches"
            case let .work(head, _, _): return "head:" + head
            }
        }
    }

    func mainQueue(fullName: String, at now: Date = Date()) -> [MainQueueItem] {
        let open = pulls.filter {
            $0.base?.ref == "main" && $0.head.ref != nil &&
                $0.head.repo?.full_name.caseInsensitiveCompare(fullName) == .orderedSame
        }
        let heads = Set(open.compactMap { $0.head.ref })
        let candidates = recent(at: now).filter { !["main", "prod"].contains($0.name) && !heads.contains($0.name) }
        var rows: [MainQueueItem] = [.openHeader(open.count)]
        for pull in open {
            if let head = pull.head.ref { rows.append(.work(head: head, pull: pull, committed: nil)) }
        }
        rows.append(.branchHeader(candidates.count))
        rows += candidates.map { .work(head: $0.name, pull: nil, committed: $0.target.committedDate) }
        return rows
    }

    func pull(head: String, base: String, fullName: String) -> Pull? {
        pulls.first { $0.head.ref == head && $0.base?.ref == base &&
            $0.head.repo?.full_name.caseInsensitiveCompare(fullName) == .orderedSame }
    }
}

struct CheckCounts {
    var passed = 0, pending = 0, failed = 0, skipped = 0

    var missingPermissions = [String]()
    var unavailable = [String]()

    mutating func count(_ state: String) {
        switch state.lowercased() {
        case "success": passed += 1
        case "neutral", "skipped": skipped += 1
        case "failure", "error", "cancelled", "timed_out", "action_required", "startup_failure", "stale": failed += 1
        default: pending += 1
        }
    }

    var total: Int { passed + failed + pending + skipped }

    var summary: String {
        let counts = [(failed, "failed"), (pending, "pending"), (passed, "passed"), (skipped, "skipped")]
            .filter { $0.0 > 0 }.map { "\($0.0) \($0.1)" }.joined(separator: ", ")
        var parts = [String]()
        if !counts.isEmpty { parts.append("Checks · " + counts) }
        if !missingPermissions.isEmpty { parts.append("Read access needed · " + missingPermissions.joined(separator: ", ")) }
        if !unavailable.isEmpty { parts.append(unavailable.joined(separator: ", ") + " unavailable") }
        return parts.isEmpty ? "No checks reported" : parts.joined(separator: " · ")
    }
}

struct PullReadiness: Decodable {
    struct Count: Decodable { let totalCount: Int }
    struct Opinions: Decodable {
        struct Review: Decodable { let state: String }
        let nodes: [Review]
        let totalCount: Int
    }
    let headRefOid: String
    let isDraft: Bool
    let state: String
    let mergeable: String
    let mergeStateStatus: String
    let reviewDecision: String?
    let reviewRequests: Count
    let latestOpinionatedReviews: Opinions
    let comments: Count

    enum Review: String {
        case changes = "Changes requested"
        case awaiting = "Awaiting review"
        case required = "Review required"
        case approvedAndRequested = "Approved · Review requested"
        case approved = "Approved"
        case requested = "Review requested"
        case submitted = "Reviews submitted"
        case commented = "Review commented"
        case none = "No active review"
    }

    enum Merge: String {
        case merged = "Merged"
        case closed = "Closed"
        case draft = "Draft"
        case conflicts = "Merge conflicts"
        case behind = "Branch behind base"
        case blocked = "Merge blocked"
        case unstable = "Checks not passing"
        case hooks = "Merge subject to hooks"
        case ready = "Ready to merge"
        case unknown = "Merge status pending"
    }

    var review: Review {
        let requested = reviewRequests.totalCount > 0
        switch reviewDecision {
        case "CHANGES_REQUESTED": return .changes
        case "REVIEW_REQUIRED": return requested ? .awaiting : .required
        case "APPROVED": return requested ? .approvedAndRequested : .approved
        default:
            let opinions = latestOpinionatedReviews.nodes.map(\.state)
            if opinions.contains("CHANGES_REQUESTED") { return .changes }
            if requested { return .requested }
            if opinions.count < latestOpinionatedReviews.totalCount { return .submitted }
            if opinions.contains("APPROVED") { return .approved }
            return comments.totalCount > 0 ? .commented : .none
        }
    }

    var merge: Merge {
        if state == "MERGED" { return .merged }
        if state == "CLOSED" { return .closed }
        if isDraft { return .draft }
        if mergeable == "CONFLICTING" { return .conflicts }
        switch mergeStateStatus {
        case "DIRTY": return .conflicts
        case "BEHIND": return .behind
        case "BLOCKED": return .blocked
        case "UNSTABLE": return .unstable
        case "HAS_HOOKS": return .hooks
        case "CLEAN" where mergeable == "MERGEABLE":
            return ["CHANGES_REQUESTED", "REVIEW_REQUIRED"].contains(reviewDecision) ? .blocked : .ready
        default: return .unknown
        }
    }
}

struct PublishedImages {
    let images: [String]
    let run: URL?
    let note: String
}

extension ConnectorAPI {
    func activity(_ repo: String, _ c: Connection) async throws -> RepoActivity {
        async let branches = recentBranches(repo, c)
        async let pulls = openPulls(repo, c)
        return try await RepoActivity(branches: branches, pulls: pulls)
    }

    // GitHub enforces one open PR per head/base pair. Resolve an existing PR on retry or a racing create.
    func ensurePull(repo: String, head: String, base: String, connection c: Connection) async throws -> Pull {
        func existing() async throws -> Pull? {
            let pulls = try await openPulls(repo, c)
            return RepoActivity(branches: [], pulls: pulls).pull(head: head, base: base, fullName: c.scopeID + "/" + repo)
        }
        if let pull = try await existing() { return pull }
        do {
            return try await createPull(repo: repo, head: head, base: base, title: "\(head) → \(base)", connection: c)
        } catch {
            if let pull = try? await existing() { return pull }
            throw error
        }
    }

    private func github<T: Decodable>(_ path: String, _ c: Connection) async throws -> T {
        let (data, _) = try await call(.github, path, token: try await OAuthCredentials.shared.token(.github, c))
        return try JSONDecoder().decode(T.self, from: data)
    }

    func pullReadiness(repo: String, pull: Pull, connection c: Connection) async throws -> PullReadiness {
        _ = try repoPath(repo, c)
        let query = """
        query($org:String!, $repo:String!, $number:Int!) {
          repository(owner:$org, name:$repo) { pullRequest(number:$number) {
            headRefOid state isDraft mergeable mergeStateStatus reviewDecision
            reviewRequests { totalCount }
            latestOpinionatedReviews(first:100) { totalCount nodes { state } }
            comments: reviews(first:1, states:[COMMENTED]) { totalCount }
          } }
        }
        """
        struct Response: Decodable {
            struct DataObject: Decodable {
                struct Repository: Decodable { let pullRequest: PullReadiness? }
                let repository: Repository?
            }
            struct Issue: Decodable { let message: String }
            let data: DataObject?
            let errors: [Issue]?
        }
        let variables: [String: Any] = ["org": c.scopeID, "repo": repo, "number": pull.number]
        let (bytes, _) = try await call(.github, "graphql", token: try await OAuthCredentials.shared.token(.github, c),
                                        method: "POST", body: ["query": query, "variables": variables])
        let result = try JSONDecoder().decode(Response.self, from: bytes)
        guard result.errors == nil, let status = result.data?.repository?.pullRequest else {
            throw ConnectorError(result.errors?.first?.message ?? "GitHub could not read this PR.")
        }
        guard status.headRefOid == pull.head.sha else { throw ConnectorError("Branch updated. Refresh the repository.") }
        return status
    }

    func checks(repo: String, pull: Pull, connection c: Connection) async throws -> CheckCounts {
        struct Run: Decodable { let id: Int; let status: String; let conclusion: String? }
        struct Runs: Decodable { let check_runs: [Run] }
        struct Status: Decodable { let context: String; let state: String }
        let path = try repoPath(repo, c) + "/commits/" + pull.head.sha
        var counts = CheckCounts(), ids = Set<Int>(), contexts = Set<String>()
        func recordError(_ error: Error, source: String) {
            if let error = error as? ConnectorError, error.integrationDenied {
                counts.missingPermissions.append(source)
            } else {
                counts.unavailable.append(source)
            }
        }
        do {
            var page = 1
            while true {
                let batch: Runs = try await github(path + "/check-runs?filter=latest&per_page=100&page=\(page)", c)
                for run in batch.check_runs where ids.insert(run.id).inserted {
                    counts.count(run.conclusion ?? run.status)
                }
                if batch.check_runs.count < 100 { break }
                page += 1
            }
        } catch {
            try Task.checkCancellation()
            recordError(error, source: "Checks")
        }
        do {
            var page = 1
            while true {
                let batch: [Status] = try await github(path + "/statuses?per_page=100&page=\(page)", c)
                for status in batch where contexts.insert(status.context.lowercased()).inserted { counts.count(status.state) }
                if batch.count < 100 { break }
                page += 1
            }
        } catch {
            try Task.checkCancellation()
            recordError(error, source: "Commit statuses")
        }
        return counts
    }

    static func publishedReferences(in log: String) -> [String] {
        let pattern = #"pushing manifest for [^\s@]+/([^\s/:@]+:[\w][\w.-]*)@sha256:[0-9a-f]{64}"#
        let regex = try! NSRegularExpression(pattern: pattern)
        let text = log as NSString
        let refs = regex.matches(in: log, range: NSRange(location: 0, length: text.length)).map { text.substring(with: $0.range(at: 1)) }
        return Set(refs).sorted()
    }

    func publishedImages(repo: String, connection c: Connection) async throws -> PublishedImages {
        struct Run: Decodable { let id: Int; let html_url: URL }
        struct Runs: Decodable { let workflow_runs: [Run] }
        struct Job: Decodable { let id: Int; let name: String; let conclusion: String? }
        struct Jobs: Decodable { let jobs: [Job] }
        let path = try repoPath(repo, c)
        // Inspect successful prod jobs. A tag is only displayed when the registry push appears in its log.
        let runs: Runs = try await github(path + "/actions/runs?branch=prod&status=success&per_page=20", c)
        for run in runs.workflow_runs {
            try Task.checkCancellation()
            let jobs: Jobs = try await github(path + "/actions/runs/\(run.id)/jobs?per_page=100", c)
            let pushes = jobs.jobs.filter { $0.conclusion == "success" && $0.name.range(of: "push|publish|docker|image|deploy", options: [.regularExpression,.caseInsensitive]) != nil }
            var images = Set<String>()
            for job in pushes {
                let token = try await OAuthCredentials.shared.token(.github, c)
                let (data, _) = try await call(.github, path + "/actions/jobs/\(job.id)/logs", token: token, downloadLog: true)
                images.formUnion(Self.publishedReferences(in: String(decoding: data, as: UTF8.self)))
            }
            if !images.isEmpty { return PublishedImages(images: images.sorted(), run: run.html_url, note: "") }
        }
        return PublishedImages(images: [], run: nil, note: "No image push found in recent successful prod runs.")
    }
}

extension ConnectorAPI {
    struct Member: Decodable, Identifiable {
        struct Profile: Decodable { let display_name: String? }
        let id: String
        let name: String
        let real_name: String?
        let profile: Profile?
        let deleted: Bool?
        let is_bot: Bool?
        var label: String {
            [profile?.display_name, real_name, name].compactMap { $0 }.first { !$0.isEmpty } ?? id
        }
    }

    func members(_ c: Connection) async throws -> [Member] {
        struct Page: Decodable {
            struct Metadata: Decodable { let next_cursor: String? }
            let members: [Member]; let response_metadata: Metadata?
        }
        var people: [Member] = [], cursor = ""
        repeat {
            var query = URLComponents()
            query.queryItems = [.init(name: "limit", value: "200"), .init(name: "cursor", value: cursor)]
            let (data, _) = try await call(.slack, "users.list?" + (query.percentEncodedQuery ?? ""),
                                           token: try await OAuthCredentials.shared.token(.slack, c))
            let page = try JSONDecoder().decode(Page.self, from: data)
            people += page.members.filter { $0.deleted != true && $0.is_bot != true && $0.id != "USLACKBOT" }
            cursor = page.response_metadata?.next_cursor ?? ""
        } while !cursor.isEmpty
        return people.sorted { $0.label.localizedCaseInsensitiveCompare($1.label) == .orderedAscending }
    }

    static func recipientID(_ text: String) -> String? {
        let input = text.trimmingCharacters(in: .whitespacesAndNewlines)
        var value = input
        if let url = URLComponents(string: input), url.scheme == "https", let host = url.host,
           host == "slack.com" || host.hasSuffix(".slack.com") {
            let parts = url.path.split(separator: "/").map(String.init)
            guard let id = parts.first(where: { $0.hasPrefix("D") }) else { return nil }
            value = id
        }
        return value.range(of: "^[DUW][A-Z0-9]+$", options: .regularExpression) == nil ? nil : value
    }

    func sendPR(_ url: URL, recipient: String, connection c: Connection) async throws {
        guard let id = Self.recipientID(recipient) else { throw ConnectorError("Choose a Slack member or enter a direct-message link in Workflow defaults.") }
        var channel = id
        if !id.hasPrefix("D") {
            struct Reply: Decodable { struct DM: Decodable { let id: String }; let channel: DM }
            let (data, _) = try await call(.slack, "conversations.open", token: try await OAuthCredentials.shared.token(.slack, c),
                                           method: "POST", body: ["users": id])
            channel = try JSONDecoder().decode(Reply.self, from: data).channel.id
            guard channel.hasPrefix("D") else { throw ConnectorError("Slack did not return a direct message.") }
        }
        try await send(url.absoluteString, channel: channel, using: c)
    }
}

extension ConnectorAPI {
    func promotionDistance(repo: String, connection c: Connection) async throws -> Int {
        struct Comparison: Decodable { let ahead_by: Int }
        let path = try repoPath(repo, c)
        let comparison: Comparison = try await github(path + "/compare/prod...main?per_page=1", c)
        return comparison.ahead_by
    }
}
