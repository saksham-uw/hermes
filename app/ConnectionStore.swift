import Foundation
import Security

struct ConnectionStore {
    let serviceName: String
    init(serviceName: String = "com.sakshamuw.hermes.connectors") { self.serviceName = serviceName }

    private func query(_ service: Service) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: serviceName, kSecAttrAccount as String: service.rawValue]
    }

    private func check(_ result: OSStatus) throws {
        guard result == errSecSuccess else {
            throw ConnectorError("Keychain: " + ((SecCopyErrorMessageString(result, nil) as String?) ?? "Error \(result)"))
        }
    }

    func read(_ service: Service) throws -> Connection? {
        var q = query(service)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var value: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &value)
        if status == errSecItemNotFound { return nil }
        try check(status)
        guard let data = value as? Data else { throw ConnectorError("Unreadable Keychain item.") }
        return try JSONDecoder().decode(Connection.self, from: data)
    }

    func save(_ c: Connection, for service: Service) throws {
        let q = query(service)
        let fields: [String: Any] = [kSecValueData as String: try JSONEncoder().encode(c), kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        let status = SecItemUpdate(q as CFDictionary, fields as CFDictionary)
        if status == errSecItemNotFound {
            try check(SecItemAdd(q.merging(fields) { _, new in new } as CFDictionary, nil))
        } else { try check(status) }
    }

    func forget(_ service: Service) throws {
        let status = SecItemDelete(query(service) as CFDictionary)
        if status != errSecItemNotFound { try check(status) }
    }
}
