import AppKit
import SwiftUI

// Keep selection and the Edit menu on the standard responder chain.
struct CopyText: NSViewRepresentable {
    let value: String
    var size: CGFloat = 14
    var face = "Inter-Regular"
    var muted = false
    var activate: (() -> Void)?

    init(_ value: String, size: CGFloat = 14, face: String = "Inter-Regular", muted: Bool = false, activate: (() -> Void)? = nil) {
        self.value = value; self.size = size; self.face = face; self.muted = muted; self.activate = activate
    }

    final class LabelView: NSTextView {
        var activate: (() -> Void)?
        private var click: DispatchWorkItem?
        override func resetCursorRects() {
            super.resetCursorRects()
            addCursorRect(visibleRect, cursor: HermesPointer.beam)
        }
        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
        override func mouseDown(with event: NSEvent) {
            click?.cancel()
            let start = NSEvent.mouseLocation
            super.mouseDown(with: event)
            let end = NSEvent.mouseLocation
            guard let activate, event.clickCount == 1, selectedRange().length == 0,
                  hypot(start.x - end.x, start.y - end.y) < 3 else { return }
            let pending = DispatchWorkItem(block: activate)
            click = pending
            DispatchQueue.main.asyncAfter(deadline: .now() + NSEvent.doubleClickInterval, execute: pending)
        }
        override func viewWillMove(toWindow newWindow: NSWindow?) {
            if newWindow == nil { click?.cancel() }
            super.viewWillMove(toWindow: newWindow)
        }
    }

    func makeNSView(context: Context) -> LabelView {
        let view = LabelView()
        view.isEditable = false
        view.isSelectable = true
        view.drawsBackground = false
        view.isRichText = false
        view.textContainerInset = .zero
        view.textContainer?.lineFragmentPadding = 0
        view.textContainer?.widthTracksTextView = true
        view.isHorizontallyResizable = false
        view.textContainer?.lineBreakMode = .byCharWrapping
        view.selectedTextAttributes = [.backgroundColor: NSColor(white: 0.76, alpha: 1), .foregroundColor: NSColor.black]
        view.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return view
    }
    func updateNSView(_ view: LabelView, context: Context) {
        view.activate = activate
        if view.string != value { view.string = value }
        let font = NSFont(name: face, size: size) ?? NSFont.systemFont(ofSize: size)
        if view.font != font { view.font = font }
        view.textColor = NSColor(white: muted ? 0.72 : 0.94, alpha: 1)
        view.setAccessibilityLabel(value)
    }
    func sizeThatFits(_ proposal: ProposedViewSize, nsView: LabelView, context: Context) -> CGSize? {
        // SwiftUI probes with zero width as well as the final proposal. Measuring the
        // live text container here would wrap or clip the next draw to a probe width.
        let font = nsView.font ?? NSFont.systemFont(ofSize: size)
        let text = value as NSString
        let attributes: [NSAttributedString.Key: Any] = [.font: font]
        let natural = ceil(text.size(withAttributes: attributes).width) + 2
        let width = max(1, min(proposal.width ?? natural, natural))
        let bounds = text.boundingRect(with: CGSize(width: width, height: .greatestFiniteMagnitude),
                                       options: [.usesLineFragmentOrigin, .usesFontLeading], attributes: attributes)
        return CGSize(width: width, height: ceil(max(font.ascender - font.descender + font.leading, bounds.height)) + 2)
    }
}

enum HermesPointer {
    static let arrow = NSCursor(image: drawing(beam: false), hotSpot: NSPoint(x: 3, y: 2))
    static let beam = NSCursor(image: drawing(beam: true), hotSpot: NSPoint(x: 10, y: 11))

    private static func drawing(beam: Bool) -> NSImage {
        NSImage(size: NSSize(width: 23, height: 25), flipped: true) { _ in
            let path = NSBezierPath()
            if beam {
                path.move(to: .init(x:6,y:3)); path.line(to:.init(x:14,y:3))
                path.move(to:.init(x:10,y:3)); path.line(to:.init(x:10,y:20))
                path.move(to:.init(x:6,y:20)); path.line(to:.init(x:14,y:20))
                NSColor.black.setStroke(); path.lineWidth = 3.5; path.stroke()
                NSColor.white.setStroke(); path.lineWidth = 1.5; path.stroke()
            } else {
                path.move(to:.init(x:3,y:2)); path.line(to:.init(x:19,y:14))
                path.line(to:.init(x:12,y:15)); path.line(to:.init(x:15,y:21))
                path.line(to:.init(x:11,y:23)); path.line(to:.init(x:8,y:16))
                path.line(to:.init(x:3,y:20)); path.close()
                NSColor(white:0.15,alpha:1).setStroke(); path.lineWidth = 2.5; path.stroke()
                NSColor(white:0.93,alpha:1).setFill(); path.fill()
            }
            return true
        }
    }
}

final class HermesAppWindow: NSWindow {
    override func sendEvent(_ event: NSEvent) {
        super.sendEvent(event)
        switch event.type {
        case .mouseMoved, .mouseEntered, .cursorUpdate, .leftMouseUp, .leftMouseDragged:
            guard isKeyWindow, let root = contentView else { return }
            let point = root.convert(event.locationInWindow, from: nil)
            guard root.bounds.insetBy(dx: 4, dy: 4).contains(point) else { return }
            let hit = root.hitTest(root.superview?.convert(event.locationInWindow, from: nil) ?? event.locationInWindow)
            let isText = hit is NSTextView || hit is NSTextField || NSCursor.current == .iBeam
            (isText ? HermesPointer.beam : HermesPointer.arrow).set()
        default: break
        }
    }
}

struct DragSurface: NSViewRepresentable {
    final class Surface: NSView {
        override func mouseDown(with event: NSEvent) { window?.performDrag(with: event) }
    }
    func makeNSView(context: Context) -> Surface { Surface() }
    func updateNSView(_ view: Surface, context: Context) {}
}

struct Hairline: View {
    var body: some View { Rectangle().fill(Theme.rule).frame(height: 1).accessibilityHidden(true) }
}

struct HermesField: View {
    let placeholder: String
    @Binding var text: String
    @FocusState private var focused: Bool
    init(_ placeholder: String, text: Binding<String>) { self.placeholder = placeholder; _text = text }
    var body: some View {
        TextField(placeholder, text: $text).textFieldStyle(.plain).focused($focused)
            .font(.custom("Inter-Regular", size: 14)).padding(10).background(Theme.canvas)
            .overlay(Rectangle().strokeBorder(focused ? Theme.ink : Theme.rule))
            .accessibilityLabel(placeholder)
    }
}

struct HermesChoices<Item: Identifiable>: View {
    let title: String
    let items: [Item]
    let label: (Item) -> String
    let choose: (Item) -> Void
    @State private var open = false
    @State private var search = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            actionButton(title + (open ? "  −" : "  +")) { open.toggle(); search = "" }
            if open {
                if items.count > 8 { HermesField("Filter", text: $search) }
                let matches = items.filter { search.isEmpty || label($0).localizedCaseInsensitiveContains(search) }
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 4) {
                        ForEach(matches) { item in
                            Button { choose(item); open = false } label: {
                                Text(verbatim: label(item)).frame(maxWidth: .infinity, alignment: .leading)
                            }.buttonStyle(ActionStyle())
                        }
                        if matches.isEmpty { Text("No matches").foregroundColor(Theme.muted) }
                    }
                }.frame(height: min(CGFloat(max(1, matches.count)) * 38, 160))
            }
        }
    }
}

struct LoadingMark: View {
    var title = "Loading…"
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    var body: some View {
        HStack(spacing: 8) {
            TimelineView(.periodic(from: .now, by: 0.32)) { tick in
                HStack(spacing: 2) {
                    ForEach(0..<3) { i in
                        Rectangle().fill(reduceMotion || Int(tick.date.timeIntervalSince1970 / 0.32) % 3 == i ? Theme.ink : Theme.rule)
                            .frame(width: 3, height: 8)
                    }
                }
            }.accessibilityHidden(true)
            Text(title).font(.custom("GeistMono-Regular", size: 11))
        }.foregroundColor(Theme.muted)
    }
}

// The two faces separate slightly on hover while the spine stays anchored.
struct RepositoryGlyph: View, Animatable {
    var spread: CGFloat = 0
    var animatableData: CGFloat { get { spread } set { spread = newValue } }
    var body: some View {
        Canvas { ctx, size in
            let scale = min(size.width, size.height) / 32
            ctx.scaleBy(x: scale, y: scale)
            for layer in [1, 0] {
                let dy = CGFloat(layer) * (4 + 2 * spread) - 1.5 * spread
                var path = Path()
                path.move(to: CGPoint(x: 5, y: 9 + dy))
                path.addLine(to: CGPoint(x: 15, y: 4 + dy))
                path.addLine(to: CGPoint(x: 28, y: 10 + dy))
                path.addLine(to: CGPoint(x: 17, y: 16 + dy))
                path.closeSubpath()
                ctx.fill(path, with: .color(Theme.surface))
                ctx.stroke(path, with: .color(layer == 0 ? Theme.ink : Theme.muted), lineWidth: 1.5)
            }
        }.accessibilityHidden(true)
    }
}
