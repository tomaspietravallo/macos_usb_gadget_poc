import SwiftUI
import Foundation
import Combine

struct USBPort: Identifiable {
    let id: String
    let status: String
    let function: String
    let link: String
    let deviceState: String
    let speed: String
    let path: String

    var ready: Bool { status == "ready" }
    var connected: Bool { link == "connected" }
}

struct HelperResult: Sendable {
    let status: Int32
    let output: String
}

enum BIOSKey: String, CaseIterable, Identifiable {
    case f1 = "F1", f2 = "F2", f3 = "F3", f4 = "F4", f5 = "F5", f6 = "F6"
    case f7 = "F7", f8 = "F8", f9 = "F9", f10 = "F10", f11 = "F11", f12 = "F12"
    case delete = "Delete", escape = "Escape"

    var id: String { rawValue }
    var hidCode: UInt8 {
        switch self {
        case .f1: 0x3a
        case .f2: 0x3b
        case .f3: 0x3c
        case .f4: 0x3d
        case .f5: 0x3e
        case .f6: 0x3f
        case .f7: 0x40
        case .f8: 0x41
        case .f9: 0x42
        case .f10: 0x43
        case .f11: 0x44
        case .f12: 0x45
        case .delete: 0x4c
        case .escape: 0x29
        }
    }
}

@MainActor
final class GadgetModel: ObservableObject {
    @Published var ports: [USBPort] = []
    @Published var selectedID: String?
    @Published var text = ""
    @Published var log = ""
    @Published var sending = false
    @Published var liveRunning = false
    @Published var liveReady = false
    @Published var livePreview = ""
    @Published var heldKeyName: String?
    @Published var selectedHoldKey: BIOSKey = .f2
    @Published var holdControl = false
    @Published var holdShift = false
    @Published var holdAlt = false

    private var liveProcess: Process?
    private var liveInput: FileHandle?
    private var liveOutput: Pipe?
    private var manuallySelected = false
    private var heldKeyCode: UInt8?
    private var heldPhysically = false

    var selectedPort: USBPort? { ports.first { $0.id == selectedID } }

    nonisolated private static func runHelper(_ arguments: [String], input: String? = nil) -> HelperResult {
        guard let executable = Bundle.main.resourceURL?.appendingPathComponent("usb_device"),
              FileManager.default.isExecutableFile(atPath: executable.path) else {
            return HelperResult(status: 1, output: "USB helper not found. Run ui/build.sh first.")
        }
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        let stdout = Pipe()
        let stderr = Pipe()
        process.standardOutput = stdout
        process.standardError = stderr
        let stdin = Pipe()
        process.standardInput = stdin
        do {
            try process.run()
            if let input, let data = input.data(using: .utf8) {
                stdin.fileHandleForWriting.write(data)
            }
            try? stdin.fileHandleForWriting.close()
            process.waitUntilExit()
            let out = stdout.fileHandleForReading.readDataToEndOfFile()
            let err = stderr.fileHandleForReading.readDataToEndOfFile()
            return HelperResult(status: process.terminationStatus,
                output: String(decoding: out + err, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines))
        } catch {
            return HelperResult(status: 1, output: error.localizedDescription)
        }
    }

    func selectPort(_ id: String) {
        selectedID = id
        manuallySelected = true
    }

    func refresh(silent: Bool = false) {
        let result = Self.runHelper(["--list"])
        if result.status != 0 {
            if !silent { log = result.output }
            return
        }
        ports = result.output.split(separator: "\n").compactMap { line in
            let fields = line.split(separator: "\t", omittingEmptySubsequences: false)
            guard fields.count == 7 else { return nil }
            return USBPort(id: String(fields[0]), status: String(fields[1]),
                function: String(fields[2]), link: String(fields[3]),
                deviceState: String(fields[4]), speed: String(fields[5]), path: String(fields[6]))
        }
        let connectedPorts = ports.filter { $0.connected }
        if !manuallySelected && !liveRunning && connectedPorts.count == 1 {
            selectedID = connectedPorts[0].id
        } else if !ports.contains(where: { $0.id == selectedID }) {
            selectedID = ports.first?.id
        }
        if !silent {
            log = ports.isEmpty ? "No USB device controllers were found." : "Found \(ports.count) USB device controllers."
        }
    }

    func send() {
        guard let port = selectedPort, port.ready, !text.isEmpty, !sending, !liveRunning else { return }
        sending = true
        log = "Sending to \(port.id)…"
        let payload = text
        Task {
            let result = await Task.detached {
                Self.runHelper(["--port", port.id], input: payload)
            }.value
            sending = false
            log = result.status == 0 ? result.output : "Send failed (exit \(result.status)):\n\(result.output)"
        }
    }

    func startLive() {
        guard let port = selectedPort, port.ready, !liveRunning, !sending,
              let executable = Bundle.main.resourceURL?.appendingPathComponent("usb_device") else { return }
        let process = Process()
        process.executableURL = executable
        process.arguments = ["--live", "--port", port.id]
        let input = Pipe()
        let output = Pipe()
        process.standardInput = input
        process.standardOutput = output
        process.standardError = output
        do {
            try process.run()
        } catch {
            log = "Could not start live keyboard: \(error.localizedDescription)"
            return
        }
        liveProcess = process
        liveInput = input.fileHandleForWriting
        liveOutput = output
        liveRunning = true
        liveReady = false
        livePreview = ""
        heldKeyName = nil
        heldKeyCode = nil
        heldPhysically = false
        log = "Connecting live keyboard to \(port.id)…"

        output.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            if data.isEmpty {
                handle.readabilityHandler = nil
                return
            }
            let chunk = String(decoding: data, as: UTF8.self)
            Task { @MainActor [weak self] in
                guard let self else { return }
                if chunk.contains("READY\n") { self.liveReady = true }
                self.log += "\n" + chunk.trimmingCharacters(in: .whitespacesAndNewlines)
            }
        }
        process.terminationHandler = { [weak self] finished in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.liveReady = false
                self.liveRunning = false
                self.heldKeyName = nil
                self.heldKeyCode = nil
                self.heldPhysically = false
                self.liveInput = nil
                self.liveProcess = nil
                self.liveOutput = nil
                self.log += "\nLive session ended (exit \(finished.terminationStatus))."
                let resultLog = self.log
                self.refresh()
                self.log = resultLog
            }
        }
    }

    func stopLive() {
        guard liveRunning else { return }
        releaseHeldKey()
        liveReady = false
        log += "\nStopping live keyboard…"
        try? liveInput?.close()
        liveInput = nil
    }

    func sendLiveKey(_ value: String) {
        guard liveReady, heldKeyName == nil, let input = liveInput else { return }
        let allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -=[]\\;',./`_+{}|:\"<>?~!@#$%^&*()\n\r\t\u{7f}\u{1b}"
        guard value.utf8.allSatisfy({ allowed.utf8.contains($0) }) else {
            log += "\nUnsupported key. Use US keyboard ASCII."
            return
        }
        do {
            try input.write(contentsOf: Data(value.utf8))
            for character in value {
                if character == "\u{7f}" || character == "\u{8}" {
                    if !livePreview.isEmpty { livePreview.removeLast() }
                } else if character == "\r" || character == "\n" {
                    livePreview += "↵ "
                } else if character == "\t" {
                    livePreview += "⇥ "
                } else {
                    livePreview.append(character)
                }
            }
            if livePreview.count > 160 { livePreview = String(livePreview.suffix(160)) }
        } catch {
            log += "\nCould not write key: \(error.localizedDescription)"
            stopLive()
        }
    }

    func holdSelectedKey() {
        var modifier: UInt8 = 0
        if holdControl { modifier |= 0x01 }
        if holdShift { modifier |= 0x02 }
        if holdAlt { modifier |= 0x04 }
        holdKey(selectedHoldKey.hidCode, modifier: modifier,
            name: selectedHoldKey.rawValue, physically: false)
    }

    func holdPhysicalKey(_ code: UInt8, modifier: UInt8, name: String) {
        holdKey(code, modifier: modifier, name: name, physically: true)
    }

    private func holdKey(_ code: UInt8, modifier: UInt8, name: String, physically: Bool) {
        guard liveReady, heldKeyName == nil, let input = liveInput else { return }
        do {
            try input.write(contentsOf: Data([1, code, modifier]))
            heldKeyName = name
            heldKeyCode = code
            heldPhysically = physically
            log += "\nHolding \(name) on \(selectedID ?? "USB controller")…"
        } catch {
            log += "\nCould not hold key: \(error.localizedDescription)"
            stopLive()
        }
    }

    func releasePhysicalKey(_ code: UInt8) {
        if heldPhysically && heldKeyCode == code { releaseHeldKey() }
    }

    func releasePhysicalHold() {
        if heldPhysically { releaseHeldKey() }
    }

    func releaseHeldKey() {
        guard heldKeyName != nil, let input = liveInput else { return }
        do {
            try input.write(contentsOf: Data([2]))
            log += "\nReleased \(heldKeyName ?? "key")."
        } catch {
            log += "\nCould not release key: \(error.localizedDescription)"
        }
        heldKeyName = nil
        heldKeyCode = nil
        heldPhysically = false
    }
}

final class KeyCaptureView: NSView {
    var enabled = false
    var onInput: ((String) -> Void)?
    var onSpecialDown: ((UInt8, UInt8, String) -> Void)?
    var onSpecialUp: ((UInt8) -> Void)?
    var onFocusLost: (() -> Void)?
    private var physicalKeyCode: UInt16?
    private var heldHIDCode: UInt8?

    override var acceptsFirstResponder: Bool { enabled }

    override func mouseDown(with event: NSEvent) {
        if enabled { window?.makeFirstResponder(self) }
    }

    override func becomeFirstResponder() -> Bool {
        needsDisplay = true
        return true
    }

    override func resignFirstResponder() -> Bool {
        onFocusLost?()
        physicalKeyCode = nil
        heldHIDCode = nil
        needsDisplay = true
        return true
    }

    private func modifierByte(for event: NSEvent) -> UInt8 {
        var value: UInt8 = 0
        if event.modifierFlags.contains(.control) { value |= 0x01 }
        if event.modifierFlags.contains(.shift) { value |= 0x02 }
        if event.modifierFlags.contains(.option) { value |= 0x04 }
        return value
    }

    private func specialCode(for event: NSEvent) -> (UInt8, String)? {
        let functionKeys: [UInt16: (UInt8, String)] = [
            122: (0x3a, "F1"), 120: (0x3b, "F2"), 99: (0x3c, "F3"),
            118: (0x3d, "F4"), 96: (0x3e, "F5"), 97: (0x3f, "F6"),
            98: (0x40, "F7"), 100: (0x41, "F8"), 101: (0x42, "F9"),
            109: (0x43, "F10"), 103: (0x44, "F11"), 111: (0x45, "F12")
        ]
        if let functionKey = functionKeys[event.keyCode] { return functionKey }
        switch event.keyCode {
        case 51: return (0x2a, "Backspace")
        case 117: return (0x4c, "Delete")
        case 53: return (0x29, "Escape")
        default: break
        }
        if event.modifierFlags.contains(.control) || event.modifierFlags.contains(.option),
           let character = event.charactersIgnoringModifiers?.lowercased().utf8.first,
           character >= 97 && character <= 122 {
            return (0x04 + character - 97, String(UnicodeScalar(character)))
        }
        return nil
    }

    override func keyDown(with event: NSEvent) {
        guard enabled, !event.modifierFlags.contains(.command) else { return }
        if let (code, name) = specialCode(for: event) {
            if physicalKeyCode == nil {
                physicalKeyCode = event.keyCode
                heldHIDCode = code
                onSpecialDown?(code, modifierByte(for: event), name)
            }
            return
        }
        switch event.keyCode {
        case 36, 76: onInput?("\n")
        case 48: onInput?("\t")
        default:
            if let characters = event.characters { onInput?(characters) }
        }
    }

    override func keyUp(with event: NSEvent) {
        if event.keyCode == physicalKeyCode, let code = heldHIDCode {
            onSpecialUp?(code)
            physicalKeyCode = nil
            heldHIDCode = nil
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.textBackgroundColor.setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8).fill()
        let focused = window?.firstResponder === self
        (focused ? NSColor.controlAccentColor : NSColor.separatorColor).setStroke()
        let border = NSBezierPath(roundedRect: bounds.insetBy(dx: 1, dy: 1), xRadius: 8, yRadius: 8)
        border.lineWidth = focused ? 2 : 1
        border.stroke()
        let message = !enabled ? "Start live typing first" :
            (focused ? "Typing live — keystrokes go to the selected USB controller" : "Click here, then type on your keyboard")
        let style: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 14),
            .foregroundColor: NSColor.secondaryLabelColor
        ]
        (message as NSString).draw(at: NSPoint(x: 16, y: bounds.midY - 9), withAttributes: style)
    }
}

struct LiveKeyCapture: NSViewRepresentable {
    let enabled: Bool
    let onInput: (String) -> Void
    let onSpecialDown: (UInt8, UInt8, String) -> Void
    let onSpecialUp: (UInt8) -> Void
    let onFocusLost: () -> Void

    func makeNSView(context: Context) -> KeyCaptureView { KeyCaptureView() }

    func updateNSView(_ view: KeyCaptureView, context: Context) {
        view.enabled = enabled
        view.onInput = onInput
        view.onSpecialDown = onSpecialDown
        view.onSpecialUp = onSpecialUp
        view.onFocusLost = onFocusLost
        view.needsDisplay = true
    }
}

@main
struct USBGadgetApp: App {
    @StateObject private var model = GadgetModel()
    private let refreshTimer = Timer.publish(every: 2, on: .main, in: .common).autoconnect()

    var body: some Scene {
        WindowGroup("USB Gadget Keyboard") {
            HStack(spacing: 0) {
                VStack(alignment: .leading, spacing: 12) {
                    Text("USB controllers")
                        .font(.headline)
                    Text("Choose the device side controller to use.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    ForEach(model.ports) { port in
                        Button {
                            model.selectPort(port.id)
                        } label: {
                            HStack {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(port.id).font(.system(.body, design: .monospaced))
                                    Text(port.connected ? "Host connected" : "No host detected")
                                        .font(.caption)
                                        .foregroundStyle(port.connected ? .green : .secondary)
                                    Text(port.ready ? "Kernel extension loaded" : "Kernel extension missing")
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                                Image(systemName: port.connected ? "cable.connector" : "circle.dashed")
                                    .foregroundStyle(port.connected ? .green : .secondary)
                            }
                            .padding(10)
                            .background(model.selectedID == port.id ? Color.accentColor.opacity(0.15) : Color.clear)
                            .clipShape(RoundedRectangle(cornerRadius: 8))
                        }
                        .buttonStyle(.plain)
                        .disabled(model.liveRunning || model.sending)
                    }
                    Spacer()
                    Button("Refresh controllers", systemImage: "arrow.clockwise") { model.refresh() }
                        .disabled(model.sending || model.liveRunning)
                }
                .frame(width: 245)
                .padding(20)
                .background(Color(nsColor: .controlBackgroundColor))

                Divider()

                ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Text("USB Gadget Keyboard")
                        .font(.largeTitle.bold())
                    Text("Select a USB controller, then type live or send a block of text to a connected computer.")
                        .foregroundStyle(.secondary)

                    if let port = model.selectedPort {
                        GroupBox("Selected interface") {
                            VStack(alignment: .leading, spacing: 8) {
                                LabeledContent("Controller", value: port.id)
                                LabeledContent("Status", value: port.ready ? "Kernel extension loaded" : "Kernel extension missing")
                                LabeledContent("Host link", value: port.connected ? "Connected" : "Not detected")
                                LabeledContent("USB state", value: port.deviceState)
                                LabeledContent("Link speed", value: port.speed)
                                LabeledContent("Current function", value: port.function)
                                LabeledContent("Registry path") {
                                    Text(port.path)
                                        .font(.system(.caption, design: .monospaced))
                                        .textSelection(.enabled)
                                }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }

                    HStack {
                        Text("Live typing").font(.headline)
                        Spacer()
                        Text(model.liveReady ? "Keyboard active" : (model.liveRunning ? "Starting…" : "Stopped"))
                            .font(.caption)
                            .foregroundStyle(model.liveReady ? .green : .secondary)
                        Button(model.liveRunning ? "Stop live typing" : "Start live typing",
                               systemImage: model.liveRunning ? "stop.fill" : "keyboard") {
                            if model.liveRunning { model.stopLive() } else { model.startLive() }
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.selectedPort?.ready != true || model.sending)
                    }
                    LiveKeyCapture(enabled: model.liveReady,
                                   onInput: { model.sendLiveKey($0) },
                                   onSpecialDown: { model.holdPhysicalKey($0, modifier: $1, name: $2) },
                                   onSpecialUp: { model.releasePhysicalKey($0) },
                                   onFocusLost: { model.releasePhysicalHold() })
                        .frame(height: 70)
                    Text("Click the box after starting. F1–F12, Delete, Backspace, Escape, and Ctrl/Alt combinations stay down until you release the physical key. macOS may intercept some F keys; use the controls below in that case.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(model.livePreview.isEmpty ? "Live preview will appear here." : model.livePreview)
                        .font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, minHeight: 28, alignment: .leading)
                        .foregroundStyle(.secondary)

                    GroupBox("Hold a BIOS key") {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Picker("Key", selection: $model.selectedHoldKey) {
                                    ForEach(BIOSKey.allCases) { key in
                                        Text(key.rawValue).tag(key)
                                    }
                                }
                                .frame(width: 180)
                                Toggle("Ctrl", isOn: $model.holdControl)
                                Toggle("Shift", isOn: $model.holdShift)
                                Toggle("Alt", isOn: $model.holdAlt)
                                Spacer()
                            }
                            .disabled(model.heldKeyName != nil)
                            HStack {
                                Button("Hold key", systemImage: "hand.point.down.fill") { model.holdSelectedKey() }
                                    .disabled(!model.liveReady || model.heldKeyName != nil)
                                Button("Release key", systemImage: "hand.raised") { model.releaseHeldKey() }
                                    .disabled(model.heldKeyName == nil)
                                Text(model.heldKeyName.map { "Holding \($0)" } ?? "No key held")
                                    .font(.caption)
                                    .foregroundStyle(model.heldKeyName == nil ? Color.secondary : Color.orange)
                                Spacer()
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }

                    Divider()
                    Text("Send a block of text")
                        .font(.headline)
                    TextEditor(text: $model.text)
                        .font(.system(.body, design: .monospaced))
                        .scrollContentBackground(.hidden)
                        .padding(6)
                        .background(Color(nsColor: .textBackgroundColor))
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(.quaternary))
                        .frame(minHeight: 80)
                    HStack {
                        Text("US keyboard ASCII • \(model.text.utf8.count)/1024 bytes")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Spacer()
                        Button(model.sending ? "Sending…" : "Send text", systemImage: "paperplane.fill") {
                            model.send()
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.sending || model.liveRunning || model.selectedPort?.ready != true || model.text.isEmpty || model.text.utf8.count > 1024)
                    }
                    Text("Connect a receiving computer first. The listed IDs are system controller names; physical port positions and delivery to the host are not verified here.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    GroupBox("Activity") {
                        ScrollView {
                            Text(model.log)
                                .font(.system(.caption, design: .monospaced))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .textSelection(.enabled)
                        }
                        .frame(height: 85)
                    }
                }
                .padding(24)
                .frame(maxWidth: .infinity, alignment: .topLeading)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .frame(minWidth: 850, minHeight: 600)
            .onAppear { model.refresh() }
            .onReceive(refreshTimer) { _ in model.refresh(silent: true) }
            .onDisappear { model.stopLive() }
        }
    }
}
