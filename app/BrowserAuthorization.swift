import AuthenticationServices
import CryptoKit

@MainActor
final class BrowserAuthorization: NSObject, ASWebAuthenticationPresentationContextProviding {
    private var session: ASWebAuthenticationSession?
    private var waiting: CheckedContinuation<URL, Error>?

    private func finish(_ result: Result<URL, Error>) {
        waiting?.resume(with: result)
        waiting = nil
        session = nil
    }

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
        let callback: URL = try await withTaskCancellationHandler {
            try Task.checkCancellation()
            return try await withCheckedThrowingContinuation { continuation in
                waiting = continuation
                let browser = ASWebAuthenticationSession(url: url, callbackURLScheme: "com.sakshamuw.hermes.auth") { url, _ in
                    Task { @MainActor in
                        if let url { self.finish(.success(url)) }
                        else { self.finish(.failure(ConnectorError("Sign-in was cancelled."))) }
                    }
                }
                browser.presentationContextProvider = self
                session = browser
                if !browser.start() { finish(.failure(ConnectorError("Could not open the authorization window."))) }
            }
        } onCancel: {
            Task { @MainActor in
                self.session?.cancel()
                self.finish(.failure(CancellationError()))
            }
        }
        try Task.checkCancellation()
        let ticket = try Self.ticket(callback, nonce: nonce)
        let data = try await OAuthServer.request(provider, "exchange", ["ticket": ticket, "verifier": verifier])
        return try JSONDecoder().decode(TokenResponse.self, from: data)
    }
}
