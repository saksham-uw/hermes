import Foundation

@main enum BrowserCallbackChecks {
    @MainActor static func main() async throws {
        let callback = "com.sakshamuw.hermes.auth://oauth/complete"
        let ticket = try BrowserAuthorization.ticket(URL(string: callback + "?state=n&ticket=result")!, nonce: "n")
        precondition(ticket == "result")
        for value in ["?state=wrong&ticket=result", "?state=n&state=n&ticket=result", "?state=n&error=denied", "?state=n"] {
            do {
                _ = try BrowserAuthorization.ticket(URL(string: callback + value)!, nonce: "n")
                fatalError("Invalid callback accepted")
            } catch is ConnectorError {}
        }
        // v0.1.6 Keychain records predate OAuth metadata.
        let legacy = Data(#"{"user":"u","scope":"s","scopeID":"id","repos":[],"token":"fixture"}"#.utf8)
        let c = try JSONDecoder().decode(Connection.self, from: legacy)
        precondition(c.grant == nil)
        let token = try await OAuthCredentials.shared.token(.slack, c)
        precondition(token == "fixture")
        print("Native callback validation and legacy connections passed.")
    }
}
