import CryptoKit
import Foundation

let args = CommandLine.arguments
let key = try Curve25519.Signing.PublicKey(rawRepresentation: Data(base64Encoded: args[2])!)
let archive = try Data(contentsOf: URL(fileURLWithPath: args[1]), options: .mappedIfSafe)
guard let signature = Data(base64Encoded: args[3]), key.isValidSignature(signature, for: archive) else {
    fatalError("Archive signature does not match the HERMES update public key")
}
