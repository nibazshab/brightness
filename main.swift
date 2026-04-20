import IOKit
import SwiftUI

typealias IOAVService = AnyObject

@_silgen_name("IOAVServiceCreateWithService")
func IOAVServiceCreateWithService(_ allocator: CFAllocator?, _ service: io_service_t) -> Unmanaged<IOAVService>?

@_silgen_name("IOAVServiceWriteI2C")
func IOAVServiceWriteI2C(_ s: IOAVService?, _ addr: UInt32, _ reg: UInt32, _ data: UnsafePointer<UInt8>, _ len: UInt32) -> Int32

struct MonDev: Identifiable {
    let id: Int
    let name: String
    let s: IOAVService
}

@MainActor
class MonManager: ObservableObject {
    @Published var mons: [MonDev] = []
    @Published var cur_idx: Int = 0
    @Published var pct: Double = 50 {
        didSet {
            let val = Int(pct)

            if !mons.isEmpty, cur_idx < mons.count {
                let service = mons[cur_idx].s

                Task.detached(priority: .userInitiated) {
                    self.set_vcp(val, on: service)
                }
            }
        }
    }

    init() {
        refresh()
    }

    func refresh() {
        var found: [MonDev] = []
        let screens = NSScreen.screens
            .filter { NSScreen.screens.count > 1 ? ($0.frame.origin != .zero) : true }
            .map(\.localizedName)

        let matching = IOServiceMatching("DCPAVServiceProxy")
        var iter: io_iterator_t = 0

        if IOServiceGetMatchingServices(0, matching, &iter) == KERN_SUCCESS {
            var cnt = 0
            while case let entry = IOIteratorNext(iter), entry != 0 {
                let loc = IORegistryEntryCreateCFProperty(entry, "Location" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? String

                if loc == "External", let s = IOAVServiceCreateWithService(kCFAllocatorDefault, entry)?.takeRetainedValue() {
                    let name = cnt < screens.count ? screens[cnt] : "Mon \(cnt + 1)"
                    found.append(MonDev(id: cnt, name: name, s: s))
                    cnt += 1
                }
                IOObjectRelease(entry)
            }
            IOObjectRelease(iter)
        }

        mons = found
        if cur_idx >= found.count { cur_idx = 0 }
    }

    private nonisolated func set_vcp(_ val: Int, on s: IOAVService) {
        var pkt: [UInt8] = [0x84, 0x03, 0x10, 0x00, UInt8(val & 0xFF), 0]
        var chk: UInt8 = 0x6E ^ 0x51
        for i in 0 ..< 5 {
            chk ^= pkt[i]
        }
        pkt[5] = chk

        _ = IOAVServiceWriteI2C(s, 0x37, 0x51, &pkt, 6)
    }
}

struct ContentView: View {
    @ObservedObject var m: MonManager

    var body: some View {
        VStack(spacing: 12) {
            if !m.mons.isEmpty {
                Picker("", selection: $m.cur_idx) {
                    ForEach(m.mons) { mon in
                        Text(mon.name).tag(mon.id)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .fixedSize()

                HStack {
                    Image(systemName: "sun.max.fill").foregroundColor(.orange)
                    Text("\(Int(m.pct))%").font(.system(.body, design: .monospaced))
                }

                Slider(value: $m.pct, in: 0 ... 100).accentColor(.orange)
            } else {
                Text("No external monitor").font(.caption).foregroundColor(.secondary)
            }

            Divider()

            Button("Exit") { NSApp.terminate(nil) }
                .buttonStyle(.plain)
                .font(.caption)
                .foregroundColor(.secondary)
                .frame(maxWidth: .infinity, alignment: .trailing)
        }
        .padding(12)
        .frame(width: 200)
        .onAppear { m.refresh() }
    }
}

@main
struct Brightness: App {
    @StateObject private var m = MonManager()

    init() {
        NSApp?.setActivationPolicy(.accessory)
    }

    var body: some Scene {
        MenuBarExtra("Br", systemImage: "sun.max") {
            ContentView(m: m)
        }.menuBarExtraStyle(.window)
    }
}
