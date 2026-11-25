// Render the H from the supplied wordmark on a macOS icon plate.
import AppKit

let output = URL(fileURLWithPath: CommandLine.arguments[1])
let plate = NSColor(srgbRed: 0.13, green: 0.14, blue: 0.16, alpha: 1)
let ink = NSColor(srgbRed: 0.94, green: 0.95, blue: 0.97, alpha: 1)
let image = NSImage(size: NSSize(width: 1024, height: 1024))
image.lockFocus()
plate.setFill()
NSBezierPath(roundedRect: NSRect(x: 90, y: 90, width: 844, height: 844),
             xRadius: 186, yRadius: 186).fill()
let lines = try String(contentsOfFile: "ascii-font.txt", encoding: .utf8)
    .components(separatedBy: .newlines).filter { !$0.isEmpty }
let style: [NSAttributedString.Key: Any] = [
    .font: NSFont.monospacedSystemFont(ofSize: 60, weight: .bold),
    .foregroundColor: ink
]
for (index, line) in lines.enumerated() {
    String(line.prefix(14)).draw(at: NSPoint(x: 250, y: 754 - index * 53), withAttributes: style)
}
image.unlockFocus()
let source = NSBitmapImageRep(data: image.tiffRepresentation!)!
try source.representation(using: .png, properties: [:])!.write(to: output)
