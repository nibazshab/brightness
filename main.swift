import IOKit
import SwiftUI

typealias IOAVService = AnyObject & Sendable

@_silgen_name("IOAVServiceCreateWithService")
func IOAVServiceCreateWithService(
    _ allocator: CFAllocator?,
    _ service: io_service_t
) -> Unmanaged<IOAVService>?

@_silgen_name("IOAVServiceWriteI2C")
func IOAVServiceWriteI2C(
    _ service: IOAVService?,
    _ addr: UInt32,
    _ reg: UInt32,
    _ data: UnsafePointer<UInt8>,
    _ len: UInt32
) -> Int32

struct MonitorDevice: Identifiable, Sendable {
    let id: Int
    let name: String
    let service: IOAVService
}

@MainActor
final class MonitorManager: ObservableObject {
    @Published var monitors: [MonitorDevice] = []
    @Published var currentIndex: Int = 0
    @Published var percentage: Double = 50 {
        didSet {
            let value = Int(percentage)
            guard monitors.indices.contains(currentIndex) else { return }
            let service = monitors[currentIndex].service

            vcpContinuation?.yield((value: value, service: service))
        }
    }

    private var vcpContinuation: AsyncStream<(value: Int, service: IOAVService)>.Continuation?
    private var workerTask: Task<Void, Never>?

    init() {
        setupVCPWorker()
        refresh()
    }

    deinit {
        workerTask?.cancel()
    }

    private func setupVCPWorker() {
        let (stream, continuation) = AsyncStream<(value: Int, service: IOAVService)>.makeStream()
        self.vcpContinuation = continuation

        self.workerTask = Task.detached(priority: .userInitiated) {
            for await item in stream {
                Self.setVCPValue(item.value, on: item.service)
            }
        }
    }

    func refresh() {
        var foundMonitors: [MonitorDevice] = []
        let allScreens = NSScreen.screens
        let externalScreenNames =
            allScreens
            .filter { allScreens.count <= 1 || $0.frame.origin != .zero }
            .map(\.localizedName)

        let matching = IOServiceMatching("DCPAVServiceProxy")
        var iterator: io_iterator_t = 0

        guard IOServiceGetMatchingServices(0, matching, &iterator) == KERN_SUCCESS else {
            return
        }

        var count = 0
        while case let entry = IOIteratorNext(iterator), entry != 0 {
            defer { IOObjectRelease(entry) }

            let location =
                IORegistryEntryCreateCFProperty(
                    entry,
                    "Location" as CFString,
                    kCFAllocatorDefault,
                    0
                )?.takeRetainedValue() as? String

            guard location == "External",
                let service = IOAVServiceCreateWithService(kCFAllocatorDefault, entry)?
                    .takeRetainedValue()
            else {
                continue
            }

            let name =
                count < externalScreenNames.count
                ? externalScreenNames[count] : "Monitor \(count + 1)"
            foundMonitors.append(MonitorDevice(id: count, name: name, service: service))
            count += 1
        }
        IOObjectRelease(iterator)

        monitors = foundMonitors
        if currentIndex >= foundMonitors.count {
            currentIndex = 0
        }
    }

    private nonisolated static func setVCPValue(_ value: Int, on service: IOAVService) {
        var packet: [UInt8] = [0x84, 0x03, 0x10, 0x00, UInt8(value & 0xFF), 0]

        var checksum: UInt8 = 0x6E ^ 0x51
        for index in 0..<5 {
            checksum ^= packet[index]
        }
        packet[5] = checksum

        _ = IOAVServiceWriteI2C(service, 0x37, 0x51, &packet, 6)
    }
}

struct ContentView: View {
    @ObservedObject var manager: MonitorManager

    var body: some View {
        VStack(spacing: 12) {
            if !manager.monitors.isEmpty {
                Picker("", selection: $manager.currentIndex) {
                    ForEach(manager.monitors) { monitor in
                        Text(monitor.name).tag(monitor.id)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .fixedSize()

                HStack {
                    Image(systemName: "sun.max.fill")
                        .foregroundColor(.orange)
                    Text("\(Int(manager.percentage))%")
                        .font(.system(.body, design: .monospaced))
                }

                Slider(value: $manager.percentage, in: 0...100)
                    .tint(.orange)
            } else {
                Text("No external monitor")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Divider()

            Button("Exit") {
                NSApp.terminate(nil)
            }
            .buttonStyle(.plain)
            .font(.caption)
            .foregroundColor(.secondary)
            .frame(maxWidth: .infinity, alignment: .trailing)
        }
        .padding(12)
        .frame(width: 200)
        .onAppear {
            manager.refresh()
        }
    }
}

@main
struct Application: App {
    @StateObject private var manager = MonitorManager()

    init() {
        NSApp?.setActivationPolicy(.accessory)
    }

    var body: some Scene {
        MenuBarExtra("Brightness", systemImage: "sun.max") {
            ContentView(manager: manager)
        }
        .menuBarExtraStyle(.window)
    }
}
