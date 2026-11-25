import Foundation

enum Service: String, CaseIterable, Identifiable, Codable {
    case slack = "Slack", github = "GitHub"
    var id: String { rawValue }
}

struct Connection: Codable {
    let user: String
    let scope: String // Slack workspace name or GitHub organization
    let scopeID: String
    let repos: [String]
    let token: String
}

struct ConnectorError: LocalizedError {
    let errorDescription: String?
    init(_ message: String) { errorDescription = message }
}

private final class NoRedirect: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) { completionHandler(nil) }
}

struct Pull: Decodable, Identifiable {
    struct Branch: Decodable { let sha: String }
    let number: Int
    let title: String
    let head: Branch
    var id: Int { number }
}

struct BranchInfo: Decodable {
    struct Target: Decodable { let committedDate: String }
    let name: String
    let target: Target
}

struct ConnectorAPI {
    private static let liveSession: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieStorage = nil
        config.urlCredentialStorage = nil
        config.timeoutIntervalForRequest = 25
        return URLSession(configuration: config, delegate: NoRedirect(), delegateQueue: nil)
    }()
    private let session: URLSession
    init(session: URLSession? = nil) { self.session = session ?? Self.liveSession }

    private func call(_ service: Service, _ path: String, token: String, method: String = "GET",
                      body: [String: Any]? = nil) async throws -> (Data, HTTPURLResponse) {
        let origin = service == .slack ? "https://slack.com/api/" : "https://api.github.com/"
        guard let url = URL(string: origin + path) else { throw ConnectorError("Invalid request.") }
        var req = URLRequest(url: url)
        req.httpMethod = method
        req.setValue("Bearer " + token, forHTTPHeaderField: "Authorization")
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        req.setValue("HERMES-macOS", forHTTPHeaderField: "User-Agent")
        if let body {
            req.setValue("application/json; charset=utf-8", forHTTPHeaderField: "Content-Type")
            req.httpBody = try JSONSerialization.data(withJSONObject: body)
        }
        let (data, raw) = try await session.data(for: req)
        guard let response = raw as? HTTPURLResponse else { throw ConnectorError("No response from " + service.rawValue + ".") }
        guard (200...299).contains(response.statusCode) else {
            let detail = (try? JSONSerialization.jsonObject(with: data) as? [String: Any])?["message"] as? String
            throw ConnectorError("\(service.rawValue) (\(response.statusCode)): \(detail ?? HTTPURLResponse.localizedString(forStatusCode: response.statusCode))")
        }
        if service == .slack {
            struct Result: Decodable { let ok: Bool; let error: String? }
            let result = try JSONDecoder().decode(Result.self, from: data)
            guard result.ok else { throw ConnectorError("Slack: " + (result.error ?? "Request failed.")) }
        }
        return (data, response)
    }

    func connectSlack(token: String, workspace: String) async throws -> Connection {
        guard token.hasPrefix("xoxp-") || token.hasPrefix("xoxe.xoxp-") else { throw ConnectorError("Use a Slack user OAuth token.") }
        struct Identity: Decodable { let team: String; let team_id: String; let user: String; let user_id: String; let bot_id: String? }
        let (data, response) = try await call(.slack, "auth.test", token: token, method: "POST")
        let me = try JSONDecoder().decode(Identity.self, from: data)
        guard me.bot_id == nil, !me.user_id.isEmpty, me.team_id == workspace else { throw ConnectorError("The token must belong to your user in the specified workspace.") }
        let scopes = (response.value(forHTTPHeaderField: "x-oauth-scopes") ?? "").split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        guard scopes.contains("chat:write") else { throw ConnectorError("The user token needs the chat:write permission.") }
        return Connection(user: me.user, scope: me.team, scopeID: me.team_id, repos: [], token: token)
    }

    func send(_ message: String, channel: String, using connection: Connection) async throws {
        guard channel.range(of: "^[CGD][A-Z0-9]+$", options: .regularExpression) != nil, !message.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw ConnectorError("Enter a channel ID and a message.")
        }
        _ = try await call(.slack, "chat.postMessage", token: connection.token, method: "POST", body: ["channel": channel, "text": message])
    }

    private func validName(_ name: String) -> Bool {
        name != "." && name != ".." && name.range(of: "^[A-Za-z0-9_.-]+$", options: .regularExpression) != nil
    }

    func connectGitHub(token: String, org: String, repos: [String]) async throws -> Connection {
        guard token.hasPrefix("github_pat_") else { throw ConnectorError("Use a fine-grained GitHub personal access token.") }
        guard validName(org), !repos.isEmpty, repos.allSatisfy(validName) else { throw ConnectorError("Enter one organization and repository names without the owner prefix.") }
        struct User: Decodable { let login: String }
        struct Repo: Decodable { struct Owner: Decodable { let type: String }; let name: String; let full_name: String; let owner: Owner }
        let (data, _) = try await call(.github, "user", token: token)
        let user = try JSONDecoder().decode(User.self, from: data)
        var verified = [String]()
        for repo in Set(repos.map { $0.lowercased() }).sorted() {
            let (data, _) = try await call(.github, "repos/\(org)/\(repo)", token: token)
            let found = try JSONDecoder().decode(Repo.self, from: data)
            guard found.owner.type == "Organization", found.full_name.lowercased() == "\(org)/\(repo)".lowercased() else { throw ConnectorError("Choose a repository owned by the specified organization.") }
            verified.append(found.name)
        }
        return Connection(user: user.login, scope: org, scopeID: org, repos: verified, token: token)
    }

    private func repoPath(_ repo: String, _ c: Connection) throws -> String {
        guard validName(c.scopeID), validName(repo), c.repos.contains(repo) else { throw ConnectorError("Repository is outside this connection.") }
        return "repos/\(c.scopeID)/\(repo)"
    }

    func recentBranches(_ repo: String, _ c: Connection) async throws -> [BranchInfo] {
        _ = try repoPath(repo, c)
        let query = """
        query($org:String!, $repo:String!, $after:String) {
          repository(owner:$org,name:$repo) { refs(refPrefix:"refs/heads/",first:100,after:$after) {
            nodes { name target { ... on Commit { committedDate } } } pageInfo { hasNextPage endCursor }
          } }
        }
        """
        struct Page: Decodable {
            struct Info: Decodable { let hasNextPage: Bool; let endCursor: String? }
            let nodes: [BranchInfo]; let pageInfo: Info
        }
        struct Result: Decodable {
            struct Payload: Decodable { struct Repo: Decodable { let refs: Page }; let repository: Repo? }
            struct Issue: Decodable { let message: String }
            let data: Payload?; let errors: [Issue]?
        }
        var branches = [BranchInfo](), cursor: String?
        repeat {
            try Task.checkCancellation()
            var vars = ["org": c.scopeID, "repo": repo]
            vars["after"] = cursor
            let (data, _) = try await call(.github, "graphql", token: c.token, method: "POST", body: ["query": query, "variables": vars])
            let r = try JSONDecoder().decode(Result.self, from: data)
            guard r.errors == nil, let page = r.data?.repository?.refs else { throw ConnectorError(r.errors?.first?.message ?? "GitHub could not list branches.") }
            branches += page.nodes
            cursor = page.pageInfo.hasNextPage ? page.pageInfo.endCursor : nil
        } while cursor != nil
        return branches.sorted { $0.target.committedDate > $1.target.committedDate }
    }

    func openPulls(_ repo: String, _ c: Connection) async throws -> [Pull] {
        let path = try repoPath(repo, c)
        var result = [Pull](), page = 1
        while true {
            let (data, _) = try await call(.github, "\(path)/pulls?state=open&per_page=100&page=\(page)", token: c.token)
            let batch = try JSONDecoder().decode([Pull].self, from: data)
            result += batch
            if batch.count < 100 { return result }
            page += 1
        }
    }

    func createPull(repo: String, head: String, base: String, title: String, connection c: Connection) async throws -> Pull {
        let path = try repoPath(repo, c)
        let (data, _) = try await call(.github, path + "/pulls", token: c.token, method: "POST", body: ["head": head, "base": base, "title": title])
        return try JSONDecoder().decode(Pull.self, from: data)
    }

    func requestReview(repo: String, pull: Pull, users: [String], connection c: Connection) async throws {
        let path = try repoPath(repo, c)
        _ = try await call(.github, "\(path)/pulls/\(pull.number)/requested_reviewers", token: c.token, method: "POST", body: ["reviewers": users])
    }

    func merge(repo: String, pull: Pull, method: String, connection c: Connection) async throws {
        let path = try repoPath(repo, c)
        let (data, _) = try await call(.github, "\(path)/pulls/\(pull.number)/merge", token: c.token, method: "PUT", body: ["sha": pull.head.sha, "merge_method": method])
        struct Result: Decodable { let merged: Bool; let message: String }
        let r = try JSONDecoder().decode(Result.self, from: data)
        guard r.merged else { throw ConnectorError(r.message) }
    }
}
