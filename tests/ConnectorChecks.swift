import Foundation

/// Run on macOS: swiftc app/ConnectorAPI.swift app/ConnectionStore.swift tests/ConnectorChecks.swift -o /tmp/hermes-connector-checks && /tmp/hermes-connector-checks
private final class Stub: URLProtocol, @unchecked Sendable {
    static var calls = 0
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
        let pull: [String: Any] = ["number": 3, "title": "Review this", "head": ["sha": "expected"]]
        switch (request.httpMethod, url.path) {
        case ("POST", "/api/auth.test"):
            headers["X-OAuth-Scopes"] = "chat:write"
            payload = ["ok": true, "user": "test-user", "user_id": "U123", "team": "Workspace", "team_id": "T123"]
        case ("POST", "/api/chat.postMessage"):
            precondition(body["channel"] as? String == "C123" && body["text"] as? String == "hello" && body["username"] == nil)
            payload = ["ok": true]
        case ("GET", "/user"): payload = ["login": "test-user"]
        case ("GET", "/repos/demo/project"): payload = ["owner": ["type": "Organization"], "name": "project", "full_name": "demo/project"]
        case ("POST", "/graphql"):
            let vars = body["variables"] as! [String: String]
            precondition(vars["repo"] == "project" && vars["org"] == "demo")
            let first = vars["after"] == nil
            let branch = ["name": first ? "old" : "new", "target": ["committedDate": first ? "2024-01-01T00:00:00Z" : "2025-02-01T00:00:00Z"]] as [String: Any]
            let page: [String: Any] = ["hasNextPage": first, "endCursor": "next"]
            payload = ["data": ["repository": ["refs": ["pageInfo": page, "nodes": [branch]]]]]
        case ("GET", "/repos/demo/project/pulls"): payload = [pull]
        case ("POST", "/repos/demo/project/pulls"):
            precondition(body["head"] as? String == "new" && body["base"] as? String == "main")
            payload = pull
        case ("POST", "/repos/demo/project/pulls/3/requested_reviewers"):
            precondition(body["reviewers"] as? [String] == ["reviewer"])
            payload = pull
        case ("PUT", "/repos/demo/project/pulls/3/merge"):
            precondition(body["sha"] as? String == "expected" && body["merge_method"] as? String == "squash")
            payload = ["merged": true, "message": "Merged"]
        default: fatalError("Unexpected request")
        }
        Self.calls += 1
        let response = HTTPURLResponse(url: url, statusCode: 200, httpVersion: nil, headerFields: headers)!
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
        let github = try await api.connectGitHub(token: "github_pat_test", org: "demo", repos: ["project", "Project"])
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

        let store = ConnectionStore(serviceName: "hermes.connector-test." + UUID().uuidString)
        defer { try? store.forget(.slack) }
        try store.save(slack, for: .slack)
        let saved = try store.read(.slack)
        precondition(saved?.token == slack.token)
        try store.forget(.slack)
        let removed = try store.read(.slack)
        precondition(removed == nil)
        print("Connector requests, identity and repository guards, and Keychain round trip passed.")
    }
}
