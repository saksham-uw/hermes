import SwiftUI

enum WorkflowMark {
    case branch, pull, merge, tick, clock, block, change, note, minus, unknown

    func icon(_ size: CGFloat = 16) -> some View {
        Canvas { ctx, bounds in
            ctx.scaleBy(x: bounds.width / 20, y: bounds.height / 20)
            var path = Path()
            func line(_ points: CGPoint...) {
                path.addLines(points)
            }
            func node(_ x: CGFloat, _ y: CGFloat) {
                path.addEllipse(in: CGRect(x: x - 2, y: y - 2, width: 4, height: 4))
            }
            switch self {
            case .branch, .pull, .merge:
                node(5, 4); node(5, 16)
                line(.init(x: 5, y: 6), .init(x: 5, y: 14))
                if self == .pull {
                    node(15, 16)
                    line(.init(x: 15, y: 14), .init(x: 15, y: 8), .init(x: 12, y: 5), .init(x: 10, y: 5))
                    line(.init(x: 12, y: 2), .init(x: 9, y: 5), .init(x: 12, y: 8))
                } else if self == .branch {
                    node(15, 4)
                    line(.init(x: 15, y: 6), .init(x: 15, y: 8), .init(x: 10, y: 13), .init(x: 5, y: 13))
                } else {
                    node(15, 16)
                    line(.init(x: 15, y: 14), .init(x: 15, y: 12), .init(x: 9, y: 6), .init(x: 5, y: 6))
                }
            case .tick, .clock, .block, .minus:
                path.addEllipse(in: CGRect(x: 2, y: 2, width: 16, height: 16))
                switch self {
                case .tick: line(.init(x: 6, y: 10), .init(x: 9, y: 13), .init(x: 14, y: 7))
                case .clock: line(.init(x: 10, y: 5), .init(x: 10, y: 10), .init(x: 14, y: 12))
                case .block: line(.init(x: 5, y: 15), .init(x: 15, y: 5))
                default: line(.init(x: 6, y: 10), .init(x: 14, y: 10))
                }
            case .change, .unknown:
                line(.init(x: 10, y: 1), .init(x: 19, y: 10), .init(x: 10, y: 19), .init(x: 1, y: 10), .init(x: 10, y: 1))
                line(.init(x: 10, y: 6), .init(x: 10, y: 10))
                line(.init(x: 10, y: 13), .init(x: 10, y: 13.5))
            case .note:
                line(.init(x: 2, y: 3), .init(x: 18, y: 3), .init(x: 18, y: 14), .init(x: 8, y: 14), .init(x: 3, y: 18), .init(x: 3, y: 14), .init(x: 2, y: 14), .init(x: 2, y: 3))
            }
            ctx.stroke(path, with: .foreground, style: StrokeStyle(lineWidth: 1.65, lineCap: .round, lineJoin: .round))
        }.frame(width: size, height: size).accessibilityHidden(true)
    }
}

private enum CheckPattern: String, CaseIterable {
    case passed = "Passed", failed = "Failed", pending = "Pending", skipped = "Skipped"

    func paint(in rect: CGRect, context: GraphicsContext) {
        var ctx = context
        ctx.clip(to: Path(rect))
        switch self {
        case .passed:
            ctx.fill(Path(rect), with: .color(Theme.ink))
        case .failed:
            var stripes = Path()
            for x in stride(from: rect.minX - rect.height, to: rect.maxX, by: 4) {
                stripes.move(to: .init(x: x, y: rect.maxY))
                stripes.addLine(to: .init(x: x + rect.height, y: rect.minY))
            }
            ctx.stroke(stripes, with: .color(Theme.ink), lineWidth: 1.6)
        case .pending:
            break // The track outline encloses the empty portion.
        case .skipped:
            var dots = Path()
            for x in stride(from: rect.minX + 1, to: rect.maxX, by: 3) {
                for y in stride(from: rect.minY + 1, to: rect.maxY, by: 3) {
                    dots.addRect(CGRect(x: x, y: y, width: 1, height: 1))
                }
            }
            ctx.fill(dots, with: .color(Theme.muted))
        }
    }
}

struct CheckPatternKey: View {
    var body: some View {
        HStack(spacing: 12) {
            ForEach(CheckPattern.allCases, id: \.self) { pattern in
                HStack(spacing: 4) {
                    Canvas { ctx, size in pattern.paint(in: CGRect(origin: .zero, size: size), context: ctx) }
                        .frame(width: 12, height: 7)
                        .overlay(Rectangle().strokeBorder(Theme.muted, lineWidth: 0.7))
                    Text(pattern.rawValue)
                }
            }
        }.font(.custom("GeistMono-Regular", size: 10)).foregroundColor(Theme.muted)
         .accessibilityElement(children: .ignore)
         .accessibilityLabel("Check patterns: solid for passed, stripes for failed, empty for pending, dots for skipped.")
    }
}

private struct ChecksMeter: View {
    let counts: CheckCounts?
    let error: String?

    var body: some View {
        HStack(spacing: 7) {
            if let counts, counts.total > 0 {
                Canvas { ctx, size in
                    var x: CGFloat = 0
                    let portions: [(CheckPattern, Int)] = [(.passed, counts.passed), (.failed, counts.failed),
                                                          (.pending, counts.pending), (.skipped, counts.skipped)]
                    for (pattern, count) in portions where count > 0 {
                        let width = size.width * CGFloat(count) / CGFloat(counts.total)
                        pattern.paint(in: CGRect(x: x, y: 0, width: width, height: size.height), context: ctx)
                        x += width
                    }
                }.frame(width: 84, height: 9)
                 .overlay(Rectangle().strokeBorder(Theme.muted, lineWidth: 0.7))
                Text("\(counts.passed)/\(counts.total)")
                if !counts.unavailable.isEmpty || !counts.missingPermissions.isEmpty { Text("Partial") }
            } else if let counts {
                if !counts.missingPermissions.isEmpty {
                    WorkflowMark.unknown.icon(13); Text("CI access")
                } else if !counts.unavailable.isEmpty {
                    WorkflowMark.unknown.icon(13); Text("CI unavailable")
                } else {
                    WorkflowMark.minus.icon(13); Text("No checks")
                }
            } else {
                WorkflowMark.clock.icon(13); Text(error == nil ? "Loading CI" : "CI unavailable")
            }
        }.fixedSize().font(.custom("GeistMono-Regular", size: 11))
         .help(counts?.summary ?? error ?? "Loading checks")
         .accessibilityElement(children: .ignore).accessibilityLabel(counts?.summary ?? error ?? "Loading checks")
    }
}

private extension PullReadiness.Review {
    var mark: WorkflowMark {
        switch self {
        case .approved: return .tick
        case .approvedAndRequested, .requested, .required, .awaiting: return .clock
        case .changes: return .change
        case .submitted, .commented: return .note
        case .none: return .minus
        }
    }
}

private extension PullReadiness.Merge {
    var appearance: (WorkflowMark, String) {
        switch self {
        case .ready: return (.merge, "Ready")
        case .blocked: return (.block, "Blocked")
        case .conflicts: return (.change, "Conflicts")
        case .unstable: return (.change, "CI unstable")
        case .behind: return (.branch, "Behind")
        case .draft: return (.note, "Draft")
        case .unknown: return (.clock, "Pending")
        case .hooks: return (.merge, "Hooks")
        case .merged: return (.merge, "Merged")
        case .closed: return (.minus, "Closed")
        }
    }
}

struct PullStatusLine: View {
    let repo: String
    let github: Connection
    let pull: Pull
    let api: ConnectorAPI
    let expanded: Bool
    @State private var readiness: PullReadiness?
    @State private var readinessError: String?
    @State private var checks: CheckCounts?
    @State private var checksError: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                ChecksMeter(counts: checks, error: checksError)
                Rectangle().fill(Theme.rule).frame(width: 1, height: 14)
                if let readiness {
                    HStack(spacing: 4) {
                        readiness.review.mark.icon()
                        Text(readiness.review.rawValue)
                    }.help("Review: " + readiness.review.rawValue)
                     .accessibilityElement(children: .ignore).accessibilityLabel("Review: " + readiness.review.rawValue)
                    let (mark, label) = readiness.merge.appearance
                    HStack(spacing: 4) {
                        mark.icon()
                        Text(label)
                    }.help(readiness.merge.rawValue)
                     .accessibilityElement(children: .ignore).accessibilityLabel(readiness.merge.rawValue)
                } else {
                    WorkflowMark.clock.icon()
                    Text(readinessError == nil ? "Loading" : "Status unavailable")
                }
            }.font(.custom("GeistMono-Regular", size: 11))
            if expanded {
                if let readiness { Text(readiness.merge.rawValue) }
                if let readinessError { Text(readinessError) }
                if let detail = checks?.summary ?? checksError { Text(detail) }
            }
        }.font(.custom("GeistMono-Regular", size: 11)).foregroundColor(Theme.muted)
         .task {
             do {
                 let result = try await api.pullReadiness(repo: repo, pull: pull, connection: github)
                 if !Task.isCancelled { readiness = result }
             } catch { if !Task.isCancelled { readinessError = error.localizedDescription } }
         }
         .task {
             do {
                 let result = try await api.checks(repo: repo, pull: pull, connection: github)
                 if !Task.isCancelled { checks = result }
             } catch { if !Task.isCancelled { checksError = error.localizedDescription } }
         }
    }
}
