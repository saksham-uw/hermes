import Foundation

enum Service: String, CaseIterable, Identifiable, Codable {
    case slack = "Slack", github = "GitHub"
    var id: String { rawValue }
}

struct Connection: Codable {
    let user: String
    var scope: String // Slack workspace name or GitHub repository owner
    var scopeID: String
    var repos: [String]
    var token: String
    var grant: OAuthGrant? = nil
    var installURL: URL? = nil
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
    struct Branch: Decodable {
        struct Repo: Decodable { let full_name: String }
        let sha: String
        let ref: String?
        let repo: Repo?
    }
    let number: Int
    let title: String
    let head: Branch
    let base: Branch?
    var id: Int { number }
}

struct BranchInfo: Decodable {
    struct Target: Decodable { let committedDate: String }
    let name: String
    let target: Target
}

struct ConnectorAPI {
    static let liveSession: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieStorage = nil
        config.urlCredentialStorage = nil
        config.timeoutIntervalForRequest = 25
        return URLSession(configuration: config, delegate: NoRedirect(), delegateQueue: nil)
    }()
    private let session: URLSession
    init(session: URLSession? = nil) { self.session = session ?? Self.liveSession }

    func call(_ service: Service, _ path: String, token: String, method: String = "GET",
                      body: [String: Any]? = nil, downloadLog: Bool = false) async throws -> (Data, HTTPURLResponse) {
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
        if downloadLog, response.statusCode == 302,
           let location = response.value(forHTTPHeaderField: "Location"), let url = URL(string: location), url.scheme == "https" {
            // Actions signs the download URL. The GitHub bearer credential stays on api.github.com.
            let (log, redirected) = try await session.data(from: url)
            guard let http = redirected as? HTTPURLResponse, http.statusCode == 200 else {
                throw ConnectorError("The production build log is unavailable.")
            }
            return (log, http)
        }
        guard (200...299).contains(response.statusCode) else {
            let payload = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            let detail = (payload?["errors"] as? [[String: Any]])?.compactMap { $0["message"] as? String }.first
                ?? payload?["message"] as? String
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
        _ = try await call(.slack, "chat.postMessage", token: try await OAuthCredentials.shared.token(.slack, connection), method: "POST", body: ["channel": channel, "text": message])
    }

    private func validName(_ name: String) -> Bool {
        name != "." && name != ".." && name.range(of: "^[A-Za-z0-9_.-]+$", options: .regularExpression) != nil
    }

    struct Installation: Decodable, Identifiable {
        struct Account: Decodable { let login: String; let type: String }
        let id: Int; let account: Account
    }

    func installations(_ token: String) async throws -> [Installation] {
        struct Page: Decodable { let installations: [Installation] }
        var all: [Installation] = [], page = 1
        while true {
            let (data, _) = try await call(.github, "user/installations?per_page=100&page=\(page)", token: token)
            let rows = try JSONDecoder().decode(Page.self, from: data).installations
            all += rows.filter { ["Organization", "User"].contains($0.account.type) && validName($0.account.login) }
            if rows.count < 100 { return all }
            page += 1
        }
    }

    func repositories(_ installation: Installation, token: String) async throws -> [String] {
        struct Repo: Decodable { let name: String; let full_name: String }
        struct Page: Decodable { let repositories: [Repo] }
        var names: [String] = [], page = 1
        while true {
            let (data, _) = try await call(.github, "user/installations/\(installation.id)/repositories?per_page=100&page=\(page)", token: token)
            let rows = try JSONDecoder().decode(Page.self, from: data).repositories
            names += rows.filter { validName($0.name) && $0.full_name.lowercased() == "\(installation.account.login)/\($0.name)".lowercased() }.map(\.name)
            if rows.count < 100 { return names.sorted() }
            page += 1
        }
    }

    func githubUser(_ token: String) async throws -> String {
        struct User: Decodable { let login: String }
        let (data, _) = try await call(.github, "user", token: token)
        return try JSONDecoder().decode(User.self, from: data).login
    }

    struct Channel: Decodable, Identifiable { let id: String; let name: String; let is_archived: Bool }
    func channels(_ c: Connection) async throws -> [Channel] {
        struct Page: Decodable {
            struct Metadata: Decodable { let next_cursor: String? }
            let channels: [Channel]; let response_metadata: Metadata?
        }
        var rows: [Channel] = [], cursor = ""
        repeat {
            var parts = URLComponents()
            parts.queryItems = [.init(name: "types", value: "public_channel,private_channel"), .init(name: "exclude_archived", value: "true"),
                                .init(name: "limit", value: "200"), .init(name: "cursor", value: cursor)]
            let path = "users.conversations?" + (parts.percentEncodedQuery ?? "")
            let (data, _) = try await call(.slack, path, token: try await OAuthCredentials.shared.token(.slack, c))
            let page = try JSONDecoder().decode(Page.self, from: data)
            rows += page.channels.filter { !$0.is_archived }
            cursor = page.response_metadata?.next_cursor ?? ""
        } while !cursor.isEmpty
        return rows.sorted { $0.name < $1.name }
    }

    func repoPath(_ repo: String, _ c: Connection) throws -> String {
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
            let (data, _) = try await call(.github, "graphql", token: try await OAuthCredentials.shared.token(.github, c), method: "POST", body: ["query": query, "variables": vars])
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
            let (data, _) = try await call(.github, "\(path)/pulls?state=open&per_page=100&page=\(page)", token: try await OAuthCredentials.shared.token(.github, c))
            let batch = try JSONDecoder().decode([Pull].self, from: data)
            result += batch
            if batch.count < 100 { return result }
            page += 1
        }
    }

    func createPull(repo: String, head: String, base: String, title: String, connection c: Connection) async throws -> Pull {
        let path = try repoPath(repo, c)
        let (data, _) = try await call(.github, path + "/pulls", token: try await OAuthCredentials.shared.token(.github, c), method: "POST", body: ["head": head, "base": base, "title": title])
        return try JSONDecoder().decode(Pull.self, from: data)
    }

    func requestReview(repo: String, pull: Pull, users: [String], connection c: Connection) async throws {
        let path = try repoPath(repo, c)
        _ = try await call(.github, "\(path)/pulls/\(pull.number)/requested_reviewers", token: try await OAuthCredentials.shared.token(.github, c), method: "POST", body: ["reviewers": users])
    }

    func merge(repo: String, pull: Pull, method: String, connection c: Connection) async throws {
        let path = try repoPath(repo, c)
        let (data, _) = try await call(.github, "\(path)/pulls/\(pull.number)/merge", token: try await OAuthCredentials.shared.token(.github, c), method: "PUT", body: ["sha": pull.head.sha, "merge_method": method])
        struct Result: Decodable { let merged: Bool; let message: String }
        let r = try JSONDecoder().decode(Result.self, from: data)
        guard r.merged else { throw ConnectorError(r.message) }
    }
}
