import Foundation

/// Run on macOS: swiftc app/ConnectorAPI.swift app/ConnectionStore.swift app/OAuthCredentials.swift app/RepositoryWorkflow.swift app/ProductionRuntime.swift tests/ConnectorChecks.swift -o /tmp/hermes-connector-checks && /tmp/hermes-connector-checks
private final class Stub: URLProtocol, @unchecked Sendable {
    static var calls = 0
    static var race = false
    static var created = 0
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() {
        let token = request.value(forHTTPHeaderField: "Authorization")
        let url = request.url!
        var payload: Any
        var headers = [String: String]()
        var data = request.httpBody ?? Data()
        if let stream = request.httpBodyStream {
            stream.open(); defer { stream.close() }
            var buf = [UInt8](repeating: 0, count: 4096)
            while stream.hasBytesAvailable {
                let n = stream.read(&buf, maxLength: buf.count)
                if n <= 0 { break }
                data.append(contentsOf: buf.prefix(n))
            }
        }
        let body = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
        if url.host == "slack.com" {
            precondition(token == "Bearer xoxp-test")
        } else {
            precondition(url.host == "api.github.com" && token == "Bearer github_pat_test")
        }
        var status = 200
        let pull: [String: Any] = ["number": 3, "title": "Review this", "head": ["sha":"expected","ref":"new","repo":["full_name":"demo/project"]], "base":["ref":"main","sha":"base"]]
        let promotion: [String: Any] = ["number": 4, "title":"Release", "head":["sha":"mainsha","ref":"main","repo":["full_name":"demo/project"]], "base":["ref":"prod","sha":"prodsha"]]
        switch (request.httpMethod, url.path) {
        case ("POST", "/api/auth.test"):
            headers["X-OAuth-Scopes"] = "chat:write"
            payload = ["ok": true, "user": "test-user", "user_id": "U123", "team": "Workspace", "team_id": "T123"]
        case ("POST", "/api/chat.postMessage"):
            if body["channel"] as? String == "D123" {
                precondition(body["text"] as? String == "https://github.com/demo/project/pull/3")
            } else { precondition(body["channel"] as? String == "C123" && body["text"] as? String == "hello") }
            precondition(body["username"] == nil)
            payload = ["ok": true]
        case ("POST", "/api/conversations.open"):
            precondition(body["users"] as? String == "U123")
            payload = ["ok":true,"channel":["id":"D123"]]
        case ("GET", "/repos/demo/project/commits/expected/check-runs"):
            payload = ["check_runs":[["id":1,"status":"completed","conclusion":"success"],["id":2,"status":"in_progress"]]]
        case ("GET", "/repos/demo/project/commits/expected/statuses"):
            payload = [["context":"lint","state":"failure"],["context":"lint","state":"pending"]]
        case ("GET", "/user"): payload = ["login": "test-user"]
        case ("GET", "/user/installations"):
            payload = ["installations": [["id": 42, "account": ["type":"Organization","login":"demo"]], ["id":43,"account":["type":"User","login":"other"]]]]
        case ("GET", "/user/installations/42/repositories"):
            payload = ["repositories":[["name":"project","full_name":"demo/project"],["name":"outside","full_name":"other/outside"]]]
        case ("GET", "/user/installations/43/repositories"):
            payload = ["repositories":[["name":"personal","full_name":"other/personal"]]]
        case ("GET", "/api/users.conversations"):
            payload = ["ok": true, "channels":[["id":"C123","name":"general","is_archived":false]]]
        case ("POST", "/graphql"):
            let vars = body["variables"] as! [String: String]
            precondition(vars["repo"] == "project" && vars["org"] == "demo")
            let first = vars["after"] == nil
            let branch = ["name": first ? "old" : "new", "target": ["committedDate": first ? "2024-01-01T00:00:00Z" : "2025-02-01T00:00:00Z"]] as [String: Any]
            let page: [String: Any] = ["hasNextPage": first, "endCursor": "next"]
            payload = ["data": ["repository": ["refs": ["pageInfo": page, "nodes": [branch]]]]]
        case ("GET", "/repos/demo/project/pulls"): payload = Self.race && Self.created > 1 ? [pull, promotion] : [pull]
        case ("POST", "/repos/demo/project/pulls"):
            Self.created += 1
            if Self.race {
                precondition(body["head"] as? String == "main" && body["base"] as? String == "prod")
                status = 422; payload = ["message":"Already open"]
            } else {
                precondition(body["head"] as? String == "new" && body["base"] as? String == "main")
                payload = pull
            }
        case ("POST", "/repos/demo/project/pulls/3/requested_reviewers"):
            precondition(body["reviewers"] as? [String] == ["reviewer"])
            payload = pull
        case ("PUT", "/repos/demo/project/pulls/3/merge"):
            precondition(body["sha"] as? String == "expected" && body["merge_method"] as? String == "squash")
            payload = ["merged": true, "message": "Merged"]
        default: fatalError("Unexpected request")
        }
        Self.calls += 1
        let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: nil, headerFields: headers)!
        client!.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client!.urlProtocol(self, didLoad: try! JSONSerialization.data(withJSONObject: payload))
        client!.urlProtocolDidFinishLoading(self)
    }
    override func stopLoading() {}
}

@main enum ConnectorChecks {
    static func main() async throws {
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [Stub.self]
        let api = ConnectorAPI(session: URLSession(configuration: config))
        do { _ = try await api.connectSlack(token: "xoxb-test", workspace: "T123"); fatalError("Accepted bot token") } catch is ConnectorError {}
        do { _ = try await api.connectSlack(token: "xoxp-test", workspace: "T999"); fatalError("Accepted different workspace") } catch is ConnectorError {}
        let slack = try await api.connectSlack(token: "xoxp-test", workspace: "T123")
        try await api.send("hello", channel: "C123", using: slack)
        let channels = try await api.channels(slack)
        precondition(channels.map(\.id) == ["C123"])
        let token = "github_pat_test"
        let installs = try await api.installations(token)
        precondition(installs.map(\.id) == [42, 43])
        let repos = try await api.repositories(installs[0], token: token)
        precondition(repos == ["project"])
        let personalRepos = try await api.repositories(installs[1], token: token)
        precondition(personalRepos == ["personal"])
        let github = Connection(user: try await api.githubUser(token), scope: "demo", scopeID: "demo", repos: repos, token: token)
        let previous = Stub.calls
        do { _ = try await api.openPulls("another", github); fatalError("Accepted unselected repo") } catch is ConnectorError {}
        precondition(previous == Stub.calls)
        let branches = try await api.recentBranches("project", github)
        precondition(branches.map(\.name) == ["new","old"])
        let pulls = try await api.openPulls("project", github)
        precondition(pulls.map(\.number) == [3])
        let p = try await api.createPull(repo: "project", head: "new", base: "main", title: "Review this", connection: github)
        try await api.requestReview(repo: "project", pull: p, users: ["reviewer"], connection: github)
        try await api.merge(repo: "project", pull: p, method: "squash", connection: github)

        let reused = try await api.ensurePull(repo: "project", head: "new", base: "main", connection: github)
        precondition(reused.number == 3 && Stub.created == 1)
        Stub.race = true
        let promotion = try await api.ensurePull(repo: "project", head: "main", base: "prod", connection: github)
        precondition(promotion.number == 4 && Stub.created == 2)
        let checks = try await api.checks(repo:"project", pull:p, connection:github)
        precondition(checks.passed == 1 && checks.pending == 1 && checks.failed == 1)
        let boundary = ISO8601DateFormatter().date(from: "2025-02-03T00:00:00Z")!
        let activity = RepoActivity(branches: branches, pulls: pulls)
        precondition(activity.recent(at: boundary).map(\.name) == ["new"])
        precondition(activity.recent(at: boundary.addingTimeInterval(1)).isEmpty)
        precondition(activity.pull(head:"new", base:"main", fullName:"another/project") == nil)
        let link = URL(string:"https://github.com/demo/project/pull/3")!
        try await api.sendPR(link, recipient:"U123", connection:slack)
        try await api.sendPR(link, recipient:"https://app.slack.com/client/T1/D123", connection:slack)
        precondition(ConnectorAPI.recipientID("C123") == nil)
        let digest = String(repeating: "a", count: 64)
        let log = "pushing manifest for registry/project:prod-123@sha256:" + digest
        precondition(ConnectorAPI.publishedReferences(in: log) == ["project:prod-123"])
        precondition(ConnectorAPI.publishedReferences(in: "build --tag project:wrong").isEmpty)
        precondition(ProductionRuntime.images(for:"project", in:"registry/project-api:live\nregistry/unrelated:wrong") == ["project-api:live"])

        let store = ConnectionStore(serviceName: "hermes.connector-test." + UUID().uuidString)
        defer { try? store.forget(.slack); try? store.forget(.github) }
        let account = Connection(user: github.user, scope: "", scopeID: "", repos: [], token: token,
                                 grant: OAuthGrant(), installURL: URL(string: "https://github.com/apps/test/installations/new"))
        try store.save(account, for: .github)
        let reopened = ConnectionStore(serviceName: store.serviceName)
        let restored = try reopened.read(.github)
        precondition(restored?.grant?.id == account.grant?.id && restored?.installURL == account.installURL && restored?.repos == [])
        var configured = account
        configured.scopeID = "demo"; configured.scope = "demo"; configured.repos = repos
        try reopened.save(configured, for: .github)
        let selected = try store.read(.github)
        precondition(selected?.repos == ["project"] && selected?.grant?.id == account.grant?.id)
        try store.save(slack, for: .slack)
        let saved = try store.read(.slack)
        precondition(saved?.token == slack.token)
        try store.forget(.slack)
        let removed = try store.read(.slack)
        precondition(removed == nil)
        print("Connector requests, identity and repository guards, and Keychain round trip passed.")
    }
}
