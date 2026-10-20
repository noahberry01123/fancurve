import SwiftUI
import AppKit

struct Fan: Identifiable {
    let id: Int
    let rpm: Double
    let min: Double
    let max: Double
    let manual: Bool
    var frac: Double { max > min ? Swift.max(0, Swift.min(1, (rpm - min) / (max - min))) : 0 }
}

@MainActor
final class FanModel: ObservableObject {
    @Published var temp: Double = 0
    @Published var fans: [Fan] = []
    @Published var mode: String = "auto"     { didSet { if !loading { apply() } } }
    @Published var percent: Double = 50       { didSet { if !loading { apply() } } }
    @Published var low: Double = 55           { didSet { if !loading { apply() } } }
    @Published var high: Double = 90          { didSet { if !loading { apply() } } }
    @Published var daemonRunning = false

    private var loading = true
    private var timer: Timer?

    init() {
        _ = FanOpen()
        loadConfig()
        loading = false
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    private func loadConfig() {
        var buf = [CChar](repeating: 0, count: 16)
        var p = 0.0, l = 0.0, h = 0.0
        ConfigReadValues(&buf, 16, &p, &l, &h)
        mode = String(cString: buf)
        percent = p; low = l; high = h
    }

    func refresh() {
        temp = MaxTemp()
        let n = Int(FanCount())
        var arr: [Fan] = []
        for i in 0..<n {
            var rpm = 0.0, mn = 0.0, mx = 0.0, tg = 0.0
            var man: Int32 = 0
            FanInfo(Int32(i), &rpm, &mn, &mx, &tg, &man)
            arr.append(Fan(id: i, rpm: rpm, min: mn, max: mx, manual: man != 0))
        }
        fans = arr
        daemonRunning = Self.isDaemonRunning()
    }

    func apply() {
        _ = mode.withCString { m in
            ConfigWriteValues(m, percent, low, high)
        }
    }

    static func isDaemonRunning() -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
        p.arguments = ["-x", "fancurved"]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = Pipe()
        do { try p.run(); p.waitUntilExit() } catch { return false }
        return p.terminationStatus == 0
    }
}

// MARK: - Views

struct FanRow: View {
    let fan: Fan
    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text("Fan \(fan.id + 1)").font(.subheadline.weight(.semibold))
                Spacer()
                Text("\(Int(fan.rpm)) rpm")
                    .font(.subheadline).monospacedDigit()
                    .foregroundStyle(.secondary)
            }
            ProgressView(value: fan.frac)
                .tint(fan.manual ? .orange : .accentColor)
            HStack {
                Text("\(Int(fan.min))").font(.caption2).foregroundStyle(.tertiary)
                Spacer()
                Text("\(Int(fan.max))").font(.caption2).foregroundStyle(.tertiary)
            }
        }
    }
}

struct PopoverView: View {
    @ObservedObject var model: FanModel

    private var tempColor: Color {
        switch model.temp {
        case ..<60:  return .green
        case 60..<80: return .yellow
        default:      return .orange
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            // Header
            HStack(spacing: 12) {
                Image(systemName: "thermometer.medium")
                    .font(.title2).foregroundStyle(tempColor)
                VStack(alignment: .leading, spacing: 0) {
                    Text("CPU / GPU").font(.caption).foregroundStyle(.secondary)
                    Text(model.temp > 0 ? "\(Int(model.temp))°C" : "—")
                        .font(.system(size: 26, weight: .semibold, design: .rounded))
                }
                Spacer()
                Image(systemName: "fanblades.fill")
                    .font(.title)
                    .foregroundStyle(.secondary)
                    .symbolEffect(.rotate, options: .repeating,
                                  isActive: (model.fans.first?.rpm ?? 0) > 0)
            }

            Divider()

            ForEach(model.fans) { FanRow(fan: $0) }

            Divider()

            Picker("", selection: $model.mode) {
                Text("Auto").tag("auto")
                Text("Manual").tag("manual")
                Text("Curve").tag("curve")
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            switch model.mode {
            case "manual":
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text("Speed").font(.subheadline)
                        Spacer()
                        Text("\(Int(model.percent))%").font(.subheadline).monospacedDigit()
                    }
                    Slider(value: $model.percent, in: 0...100, step: 1)
                    Text("Each fan runs at \(Int(model.percent))% of its own range.")
                        .font(.caption2).foregroundStyle(.secondary)
                }
            case "curve":
                VStack(alignment: .leading, spacing: 6) {
                    Stepper("Fans start at \(Int(model.low))°C",
                            value: $model.low, in: 30...80)
                    Stepper("Full speed at \(Int(model.high))°C",
                            value: $model.high, in: 60...105)
                }
                .font(.subheadline)
            default:
                Text("Fans are controlled by macOS.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Divider()

            HStack {
                Circle()
                    .fill(model.daemonRunning ? Color.green : Color.orange)
                    .frame(width: 7, height: 7)
                Text(model.daemonRunning ? "Controller active" : "Controller not running")
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Quit") { NSApplication.shared.terminate(nil) }
                    .buttonStyle(.borderless)
            }
        }
        .padding(16)
        .frame(width: 300)
    }
}

@main
struct FanCurveApp: App {
    @StateObject private var model = FanModel()

    var body: some Scene {
        // Main window — gives the app a Dock icon and app-switcher entry.
        Window("Fan Curve", id: "main") {
            PopoverView(model: model)
        }
        .windowResizability(.contentSize)

        // Menu-bar item — quick access without opening the window.
        MenuBarExtra {
            PopoverView(model: model)
        } label: {
            let label = model.temp > 0 ? "\(Int(model.temp))°" : "—"
            Image(systemName: "fanblades.fill")
            Text(label)
        }
        .menuBarExtraStyle(.window)
    }
}
