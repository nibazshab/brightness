import SwiftUI
import IOKit

typealias IOAVService = AnyObject
@_silgen_name("IOAVServiceCreateWithService")
func IOAVServiceCreateWithService(_ allocator: CFAllocator?, _ service: io_service_t) -> Unmanaged<IOAVService>?
@_silgen_name("IOAVServiceWriteI2C")
func IOAVServiceWriteI2C(_ service: IOAVService?, _ chipAddress: UInt32, _ dataAddress: UInt32, _ data: UnsafePointer<UInt8>, _ dataLength: UInt32) -> Int32

struct MonitorDevice: Identifiable {
    let id: Int
    let name: String
    let service: IOAVService
}

class BrightnessManager: ObservableObject {
    @Published var monitors: [MonitorDevice] = []
    @Published var selectedMonitorIndex: Int = 0
    @Published var brightness: Double = 50 {
        didSet {
            let val = Int(brightness)

            DispatchQueue.global(qos: .userInitiated).async {
                self.setDDCBrightness(val)
            }
        }
    }

    init() { refreshMonitors() }

func refreshMonitors() {
    var found: [MonitorDevice] = []

    let externalScreenNames = NSScreen.screens
        .filter { screen in
            return NSScreen.screens.count > 1 ? (screen.frame.origin != .zero) : true
        }
        .map { $0.localizedName }

    let matching = IOServiceMatching("DCPAVServiceProxy")
    var iter: io_iterator_t = 0
    
    if IOServiceGetMatchingServices(0, matching, &iter) == KERN_SUCCESS {
        var count = 0
        while case let entry = IOIteratorNext(iter), entry != 0 {
            let loc = IORegistryEntryCreateCFProperty(entry, "Location" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? String
            
            if loc == "External" {
                if let s = IOAVServiceCreateWithService(kCFAllocatorDefault, entry)?.takeRetainedValue() {

                    var displayName = "显示器 \(count + 1)"
                    if count < externalScreenNames.count {
                        displayName = externalScreenNames[count]
                    }
                    
                    found.append(MonitorDevice(id: count, name: displayName, service: s))
                    count += 1
                }
            }
            IOObjectRelease(entry)
        }
        IOObjectRelease(iter)
    }

    DispatchQueue.main.async {
        self.monitors = found
        if self.selectedMonitorIndex >= found.count {
            self.selectedMonitorIndex = 0
        }
    }
}

    private func setDDCBrightness(_ value: Int) {
        guard !monitors.isEmpty, selectedMonitorIndex < monitors.count else { return }
        let service = monitors[selectedMonitorIndex].service
        var packet: [UInt8] = [0x84, 0x03, 0x10, 0x00, UInt8(value & 0xFF), 0]
        var chk: UInt8 = 0x6E ^ 0x51
        for i in 0..<5 { chk ^= packet[i] }
        packet[5] = chk
        _ = IOAVServiceWriteI2C(service, 0x37, 0x51, &packet, 6)
    }
}

struct ContentView: View {
    @ObservedObject var manager: BrightnessManager
    
    var body: some View {
        VStack(spacing: 12) {
            if !manager.monitors.isEmpty {
            
                Picker("", selection: $manager.selectedMonitorIndex) {
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
                    Text("\(Int(manager.brightness))%")
                        .font(.system(.body, design: .monospaced))
                }
                
                Slider(value: $manager.brightness, in: 0...100)
                    .accentColor(.orange)
            } else {
                Text("未发现外接显示器")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            
            Divider()
            
            Button("退出") {
                NSApplication.shared.terminate(nil)
            }
            .buttonStyle(.plain)
            .font(.caption)
            .foregroundColor(.secondary)
            .frame(maxWidth: .infinity, alignment: .trailing)
        }
        .padding(12)
        .frame(width: 200)
        .onAppear {
            manager.refreshMonitors()
        }
    }
}

@main
struct BrightnessApp: App {
    @StateObject private var manager = BrightnessManager()
    init() { NSApp?.setActivationPolicy(.accessory) }
    var body: some Scene {
        MenuBarExtra("Brightness", systemImage: "sun.max") {
            ContentView(manager: manager)
        }
        .menuBarExtraStyle(.window)
    }
}
