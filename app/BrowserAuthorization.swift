import AuthenticationServices
import CryptoKit

@MainActor
final class BrowserAuthorization: NSObject, ASWebAuthenticationPresentationContextProviding {
    private var session: ASWebAuthenticationSession?
    func presentationAnchor(for session: ASWebAuthenticationSession) -> ASPresentationAnchor {
        NSApp.keyWindow ?? NSApp.windows.first!
    }
    private func random() -> String {
        var rng = SystemRandomNumberGenerator()
        return Self.encode(Data((0..<32).map { _ in UInt8.random(in: 0...255, using: &rng) }))
    }
    static func encode(_ bytes: Data) -> String {
        bytes.base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
    }
    static func ticket(_ url: URL, nonce: String) throws -> String {
        let parts = URLComponents(url: url, resolvingAgainstBaseURL: false)
        let items = parts?.queryItems ?? []
        guard parts?.scheme == "com.sakshamuw.hermes.auth", parts?.host == "oauth", parts?.path == "/complete",
              items.filter({ $0.name == "state" }).map(\.value) == [nonce] else { throw ConnectorError("Unexpected authorization callback.") }
        guard !items.contains(where: { $0.name == "error" }) else { throw ConnectorError("Authorization was declined.") }
        guard let ticket = items.first(where: { $0.name == "ticket" })?.value, !ticket.isEmpty else { throw ConnectorError("Missing authorization result.") }
        return ticket
    }
    func authorize(_ provider: Service) async throws -> TokenResponse {
        _ = try await OAuthServer.request(provider, "ready")
        let verifier = random(), nonce = random()
        let challenge = Self.encode(Data(SHA256.hash(data: Data(verifier.utf8))))
        let url = OAuthServer.endpoint(provider, "start", extra: [.init(name: "nonce", value: nonce), .init(name: "challenge", value: challenge)])
        let callback: URL = try await withCheckedThrowingContinuation { continuation in
            let browser = ASWebAuthenticationSession(url: url, callbackURLScheme: "com.sakshamuw.hermes.auth") { url, _ in
                if let url { continuation.resume(returning: url) }
                else { continuation.resume(throwing: ConnectorError("Sign-in was cancelled.")) }
            }
            browser.presentationContextProvider = self
            session = browser
            if !browser.start() { continuation.resume(throwing: ConnectorError("Could not open the authorization window.")) }
        }
        session = nil
        let ticket = try Self.ticket(callback, nonce: nonce)
        let data = try await OAuthServer.request(provider, "exchange", ["ticket": ticket, "verifier": verifier])
        return try JSONDecoder().decode(TokenResponse.self, from: data)
    }
}
