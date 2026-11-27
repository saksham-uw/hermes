import Foundation

struct OAuthGrant: Codable {
    var id = UUID()
    var refresh: String?
    var expires: Date?
}

struct TokenResponse: Decodable {
    let token: String
    let refresh: String?
    let expires: TimeInterval?
    let workspace: String?
    let install: URL?
    var grant: OAuthGrant { OAuthGrant(refresh: refresh, expires: expires.map { Date().addingTimeInterval($0) }) }
}

enum OAuthServer {
    static func endpoint(_ provider: Service, _ action: String, extra: [URLQueryItem] = []) -> URL {
        var url = URLComponents(string: "https://hermes-mac-saksham.vercel.app/api/oauth")!
        url.queryItems = [.init(name: "provider", value: provider.rawValue.lowercased()), .init(name: "action", value: action)] + extra
        return url.url!
    }
    static func request(_ provider: Service, _ action: String, _ body: [String: String]? = nil) async throws -> Data {
        var r = URLRequest(url: endpoint(provider, action))
        r.cachePolicy = .reloadIgnoringLocalCacheData
        if let body {
            r.httpMethod = "POST"
            r.setValue("application/json", forHTTPHeaderField: "Content-Type")
            r.httpBody = try JSONEncoder().encode(body)
        }
        let (data, response) = try await ConnectorAPI.liveSession.data(for: r)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard status == 200 else {
            if status == 503 || status == 404 { throw ConnectorError("\(provider.rawValue) sign-in hasn’t been enabled for HERMES yet.") }
            throw ConnectorError("Authorization failed. Connect to \(provider.rawValue) again.")
        }
        return data
    }
}

@MainActor
final class OAuthCredentials {
    static let shared = OAuthCredentials()
    private var pending: [UUID: Task<String, Error>] = [:]
    func token(_ service: Service, _ connection: Connection) async throws -> String {
        guard let grant = connection.grant else { return connection.token } // Existing manual connections.
        let store = ConnectionStore()
        guard var current = try store.read(service), var saved = current.grant, saved.id == grant.id else {
            throw ConnectorError("Connection changed. Reopen Connectors.")
        }
        guard let expires = saved.expires, expires.timeIntervalSinceNow < 90 else { return current.token }
        guard let refresh = saved.refresh else { throw ConnectorError("Authorization expired. Reconnect your account.") }
        if let existing = pending[grant.id] { return try await existing.value }
        let task = Task<String, Error> {
            let data = try await OAuthServer.request(service, "refresh", ["refresh": refresh])
            let response = try JSONDecoder().decode(TokenResponse.self, from: data)
            guard try store.read(service)?.grant?.id == saved.id else { throw ConnectorError("Connection changed during renewal.") }
            current.token = response.token
            saved.refresh = response.refresh ?? refresh
            saved.expires = response.grant.expires
            current.grant = saved
            try store.save(current, for: service)
            return response.token
        }
        pending[grant.id] = task
        defer { pending[grant.id] = nil }
        return try await task.value
    }
}
