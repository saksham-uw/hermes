import Foundation

enum ProductionRuntime {
    static func images(for repo: String, in listing: String) -> [String] {
        let names = listing.split(whereSeparator: \.isNewline).map { String($0.split(separator: "/").last ?? $0) }
        return Set(names.filter { image in
            let repository = image.prefix { $0 != ":" && $0 != "@" }
            return repository == repo || repository.hasPrefix(repo + "-")
        }).sorted()
    }

    static func runningImages(context: String, repo: String) async throws -> [String] {
        guard !context.isEmpty else { return [] }
        return try await Task.detached(priority: .utility) {
            let executables = ["/opt/homebrew/bin/docker", "/usr/local/bin/docker", "/Applications/Docker.app/Contents/Resources/bin/docker"]
            guard let docker = executables.first(where: FileManager.default.isExecutableFile(atPath:)) else {
                throw ConnectorError("Install the Docker CLI to read the production context.")
            }
            let process = Process(), output = Pipe()
            process.executableURL = URL(fileURLWithPath: docker)
            process.arguments = ["--context", context, "ps", "--format", "{{.Image}}"]
            process.environment = ProcessInfo.processInfo.environment.merging(["PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"]) { _, new in new }
            process.standardInput = FileHandle.nullDevice
            process.standardOutput = output
            process.standardError = FileHandle.nullDevice
            try process.run()
            DispatchQueue.global().asyncAfter(deadline: .now() + 15) {
                if process.isRunning { process.terminate() }
            }
            let data = output.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            guard process.terminationStatus == 0 else { throw ConnectorError("Could not read Docker context “\(context)”.") }
            return images(for: repo, in: String(decoding: data, as: UTF8.self))
        }.value
    }
}
