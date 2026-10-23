import Cocoa
import IOKit
import IOKit.hid

/// 本体の加速度センサの揺れを、デバッグ表示（`DeveloperOverlay`）にグラフで出す（開発者モード・試験的。2026-10-24）。
///
/// 自分の打鍵の強さを見るためのもの。MacBook（M2 以降）の非公開のセンサ（AppleSPUHIDDevice、
/// 用途ページ 0xFF00・用途 3）を読む。手順は https://github.com/olvvier/apple-silicon-accelerometer に合わせたが、
/// 同ライブラリの説明と違い管理者権限は要らない（2026-10-24 に MacBook で、一般ユーザの権限で起こす設定の書き込み・
/// 装置を開く・797 件/秒の受信まで確認。`experiments/keystroke-force/` の `probe`）。
/// 非公開の経路なので、macOS の更新で読めなくなることがある。
///
/// **外すとき**はこのファイル、`DeveloperOverlay` のグラフ（`graph`）、`DeveloperOverlaySettings.showsAccelerometer`、
/// 設定画面のトグル、`IrohaInputController.handleKey` の `noteKeyDown`、センサを使うタップ入力（`TapInput.swift`・
/// IrohaCore の `TapGestureDetector`・設定画面の「タップ入力」・`main.swift` の `TapInput.shared.start()`）を消す。
///
/// センサの値は専用のスレッドの実行ループで受ける（約 800 件/秒。IME のメインスレッドを起こさないため）。
/// 起こす設定（`wakeProperties`）はこのセンサの下のドライバにだけ書く。止めるときは報告間隔だけ元に戻す
/// （ほかの 2 つは元の値を読めないので戻さない）
/// 加速度センサを利用するか（設定 > 情報 > 開発者向け。試験的・既定OFF・同期しない）。
/// 使うのはデバッグ表示のグラフとタップ入力。開発者モードが OFF のとき・センサのない Mac では、設定が ON で残っていても使わない
enum AccelerometerSettings {
    /// 2026-10-24 まではデバッグ表示のグラフだけの設定だったので、キーの名前はそのときのもの
    static let enabledKey = "developerOverlayAccelerometer"

    static var isEnabled: Bool {
        DeveloperModeSettings.isEnabled && KeystrokeAccelerometer.isSensorPresent
            && UserDefaults.standard.bool(forKey: enabledKey)
    }
}

final class KeystrokeAccelerometer: @unchecked Sendable {
    static let shared = KeystrokeAccelerometer()

    struct Sample {
        var t: Double
        /// 重力・姿勢の成分を引いた揺れの大きさ（mg）
        var magnitude: Float
    }

    enum State: Equatable {
        case stopped
        case running
        /// 使えない（理由）。設定を OFF にするまで開き直さない（毎打鍵で探し直さないため）
        case unavailable(String)
    }

    // MARK: - メインスレッドで触る状態

    private(set) var state: State = .stopped
    /// 直近にキーを押した時刻（グラフに縦線を引く）
    private(set) var keyTimes: [Double] = []

    private var device: IOHIDDevice?
    private var drivers: [io_service_t] = []
    private var savedReportIntervals: [(io_service_t, Int)] = []
    private var sensorRunLoop: CFRunLoop?

    // MARK: - センサのスレッドが書き、描画が読む（`lock` で守る）

    private static let capacity = 4096  // 約 5 秒ぶん
    private let lock = NSLock()
    private var ring = [Sample](repeating: Sample(t: 0, magnitude: 0), count: capacity)
    private var ringEnd = 0
    private var ringCount = 0
    private var baseline: (x: Double, y: Double, z: Double)?
    private var lastSampleTime: Double?
    /// 揺れを 1 件受け取るたびにセンサのスレッドで呼ぶ（タップ入力）
    private var sampleObserver: ((_ t: Double, _ magnitude: Float) -> Void)?

    private static let vendorUsagePage = 0xFF00
    private static let accelerometerUsage = 3
    private static let reportLength = 22
    /// センサのドライバに頼む報告間隔（マイクロ秒）
    private static let reportInterval: Int32 = 1000
    private static let wakeProperties: [(String, Int32)] = [
        ("SensorPropertyReportingState", 1),
        ("SensorPropertyPowerState", 1),
        ("ReportInterval", reportInterval),
    ]
    /// 重力・姿勢を引くための移動平均の時定数（秒）。打鍵の衝撃（数十 ms）より十分長くする
    private static let baselineTimeConstant = 0.3
    /// グラフに残すキーの時間（秒）
    private static let keyRetention = 5.0

    private init() {}

    // MARK: - 開始・停止

    /// 設定に合わせて開く・閉じる（キーを押したときと、設定が変わったとき（`TapInput`）に呼ぶ）。メインスレッドから呼ぶ
    func syncWithSettings() {
        let wanted = AccelerometerSettings.isEnabled
        switch (wanted, state) {
        case (true, .stopped): start()
        case (false, .running): stop()
        case (false, .unavailable): state = .stopped
        default: break
        }
    }

    /// センサがこの Mac にあるか（権限は要らない）。機械に付いているものなので起動中は変わらず、最初に1回だけ調べる
    static let isSensorPresent: Bool = {
        let devices = accelerometerDevices()
        devices.forEach { IOObjectRelease($0) }
        return !devices.isEmpty
    }()

    private func start() {
        let devices = Self.accelerometerDevices()
        guard let service = devices.first else {
            state = .unavailable("この Mac には加速度センサがない（M2 以降の MacBook のみ）")
            return
        }
        devices.dropFirst().forEach { IOObjectRelease($0) }
        defer { IOObjectRelease(service) }

        drivers = Self.drivers(under: service)
        for driver in drivers {
            if let original = Self.intProperty(driver, "ReportInterval") {
                savedReportIntervals.append((driver, original))
            }
            for (key, value) in Self.wakeProperties {
                IORegistryEntrySetCFProperty(driver, key as CFString, NSNumber(value: value))
            }
        }
        guard let hid = IOHIDDeviceCreate(kCFAllocatorDefault, service) else {
            restoreDrivers()
            state = .unavailable("センサを開けない（IOHIDDeviceCreate）")
            return
        }
        let result = IOHIDDeviceOpen(hid, IOOptionBits(kIOHIDOptionsTypeNone))
        guard result == kIOReturnSuccess else {
            restoreDrivers()
            state = .unavailable(String(format: "センサを開けない（0x%08x）", result))
            return
        }
        device = hid
        resetSamples()

        let ready = DispatchSemaphore(value: 0)
        let context = Unmanaged.passUnretained(self).toOpaque()
        nonisolated(unsafe) var runLoop: CFRunLoop?
        let thread = Thread {
            let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: 4096)
            IOHIDDeviceRegisterInputReportWithTimeStampCallback(hid, buffer, 4096, accelerometerReportCallback, context)
            IOHIDDeviceScheduleWithRunLoop(hid, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
            runLoop = CFRunLoopGetCurrent()
            ready.signal()
            CFRunLoopRun()
            // `stop` が装置を閉じて実行ループを止めたあと
            buffer.deallocate()
        }
        thread.name = "iroha.accelerometer"
        thread.qualityOfService = .utility
        thread.start()
        ready.wait()
        sensorRunLoop = runLoop
        state = .running
    }

    private func stop() {
        if let hid = device, let runLoop = sensorRunLoop {
            CFRunLoopPerformBlock(runLoop, CFRunLoopMode.defaultMode.rawValue) {
                IOHIDDeviceUnscheduleFromRunLoop(hid, runLoop, CFRunLoopMode.defaultMode.rawValue)
                IOHIDDeviceClose(hid, IOOptionBits(kIOHIDOptionsTypeNone))
                CFRunLoopStop(runLoop)
            }
            CFRunLoopWakeUp(runLoop)
        }
        device = nil
        sensorRunLoop = nil
        restoreDrivers()
        keyTimes = []
        state = .stopped
    }

    private func restoreDrivers() {
        for (driver, original) in savedReportIntervals {
            IORegistryEntrySetCFProperty(driver, "ReportInterval" as CFString, NSNumber(value: Int32(original)))
        }
        savedReportIntervals = []
        drivers.forEach { IOObjectRelease($0) }
        drivers = []
    }

    // MARK: - キー

    /// キーを押した（IME に届いたキー）。メインスレッドから呼ぶ
    func noteKeyDown(_ event: NSEvent) {
        syncWithSettings()
        guard state == .running else { return }
        // NSEvent の時刻は起動からの秒で、センサの時刻（mach_absolute_time）と同じ時計。違えば今の時刻にする
        let now = machSeconds(mach_absolute_time())
        let t = abs(event.timestamp - now) < 1 ? event.timestamp : now
        keyTimes.append(t)
        keyTimes.removeAll { $0 < now - Self.keyRetention }
    }

    // MARK: - サンプル

    private func resetSamples() {
        lock.lock()
        ringEnd = 0
        ringCount = 0
        baseline = nil
        lastSampleTime = nil
        lock.unlock()
    }

    /// 揺れを受け取るたびに呼ぶ処理を置く（nil で外す）。処理はセンサのスレッドで動くので、そのスレッドだけで状態を持つこと
    func setSampleObserver(_ observer: ((_ t: Double, _ magnitude: Float) -> Void)?) {
        lock.lock()
        sampleObserver = observer
        lock.unlock()
    }

    /// センサのスレッドから呼ばれる
    fileprivate func append(t: Double, x: Double, y: Double, z: Double) {
        lock.lock()
        var observed: (observer: (Double, Float) -> Void, magnitude: Float)?
        defer {
            lock.unlock()
            observed.map { $0.observer(t, $0.magnitude) }
        }
        var base = baseline ?? (x, y, z)
        if let last = lastSampleTime {
            let k = min(1, max(0, (t - last) / Self.baselineTimeConstant))
            base.x += (x - base.x) * k
            base.y += (y - base.y) * k
            base.z += (z - base.z) * k
        }
        baseline = base
        lastSampleTime = t
        let dx = x - base.x, dy = y - base.y, dz = z - base.z
        let magnitude = Float((dx * dx + dy * dy + dz * dz).squareRoot() * 1000)
        ring[ringEnd] = Sample(t: t, magnitude: magnitude)
        if let sampleObserver { observed = (sampleObserver, magnitude) }
        ringEnd = (ringEnd + 1) % Self.capacity
        ringCount = min(ringCount + 1, Self.capacity)
    }

    /// 時刻 `since` 以降のサンプル（古い順）
    func samples(since: Double) -> [Sample] {
        lock.lock()
        defer { lock.unlock() }
        var result: [Sample] = []
        var index = (ringEnd - ringCount + Self.capacity) % Self.capacity
        for _ in 0..<ringCount {
            let sample = ring[index]
            if sample.t >= since { result.append(sample) }
            index = (index + 1) % Self.capacity
        }
        return result
    }

    // MARK: - IOKit

    private static func intProperty(_ service: io_service_t, _ key: String) -> Int? {
        (IORegistryEntryCreateCFProperty(service, key as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue() as? NSNumber)?.intValue
    }

    private static func accelerometerDevices() -> [io_service_t] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AppleSPUHIDDevice"), &iterator)
            == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }
        var result: [io_service_t] = []
        while case let service = IOIteratorNext(iterator), service != 0 {
            if intProperty(service, "PrimaryUsagePage") == vendorUsagePage,
               intProperty(service, "PrimaryUsage") == accelerometerUsage {
                result.append(service)
            } else {
                IOObjectRelease(service)
            }
        }
        return result
    }

    /// 装置の下（サービスの木をたどる）にある AppleSPUHIDDriver
    private static func drivers(under device: io_service_t) -> [io_service_t] {
        var iterator: io_iterator_t = 0
        guard IORegistryEntryCreateIterator(
            device, kIOServicePlane, IOOptionBits(kIORegistryIterateRecursively), &iterator) == KERN_SUCCESS
        else { return [] }
        defer { IOObjectRelease(iterator) }
        var result: [io_service_t] = []
        while case let service = IOIteratorNext(iterator), service != 0 {
            if IOObjectConformsTo(service, "AppleSPUHIDDriver") != 0 {
                result.append(service)
            } else {
                IOObjectRelease(service)
            }
        }
        return result
    }
}

private let machTimebase: mach_timebase_info_data_t = {
    var info = mach_timebase_info_data_t()
    mach_timebase_info(&info)
    return info
}()

/// mach_absolute_time の刻みを秒に（センサの時刻と同じ時計。タップ入力も使う）
func machSeconds(_ ticks: UInt64) -> Double {
    Double(ticks) * Double(machTimebase.numer) / Double(machTimebase.denom) / 1e9
}

/// 加速度の HID レポート: 22 バイト、x/y/z は 6・10・14 バイト目からの int32（リトルエンディアン、Q16 で単位 g）
private let accelerometerReportCallback: IOHIDReportWithTimeStampCallback = {
    context, _, _, _, _, report, length, timestamp in
    guard length == 22, let context else { return }
    func axis(_ offset: Int) -> Double {
        var value: Int32 = 0
        memcpy(&value, report + offset, 4)
        return Double(Int32(littleEndian: value)) / 65536
    }
    Unmanaged<KeystrokeAccelerometer>.fromOpaque(context).takeUnretainedValue()
        .append(t: machSeconds(timestamp), x: axis(6), y: axis(10), z: axis(14))
}

/// デバッグ表示の中に出す、直近 2 秒の揺れのグラフ。キーを押した時刻に縦線を引く。
/// 窓が小さく文字を添えても読めないので、文字は出さずグラフだけにする（2026-10-24）
final class AccelerometerGraphView: NSView {
    static let size = NSSize(width: 320, height: 36)
    /// 横軸に出す時間（秒）
    private static let span = 2.0
    /// 縦軸の上端の最小（mg）。これより大きい揺れがあれば上端を広げる
    private static let minimumTop: Float = 50

    override init(frame: NSRect) {
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    override func draw(_ dirtyRect: NSRect) {
        let sensor = KeystrokeAccelerometer.shared
        let plot = bounds

        // 軸
        NSColor.separatorColor.setStroke()
        let axis = NSBezierPath()
        axis.move(to: NSPoint(x: plot.minX, y: plot.minY))
        axis.line(to: NSPoint(x: plot.maxX, y: plot.minY))
        axis.lineWidth = 0.5
        axis.stroke()

        guard sensor.state == .running else { return }
        let samples = sensor.samples(since: machSeconds(mach_absolute_time()) - Self.span - 0.2)
        guard let now = samples.last?.t else { return }
        let start = now - Self.span
        let visible = samples.filter { $0.t >= start }
        let top = max(Self.minimumTop, visible.map(\.magnitude).max() ?? 0)
        func x(_ t: Double) -> CGFloat { plot.minX + CGFloat((t - start) / Self.span) * plot.width }
        func y(_ v: Float) -> CGFloat { plot.minY + CGFloat(min(v, top) / top) * plot.height }

        // キーの縦線
        NSColor.systemRed.withAlphaComponent(0.6).setStroke()
        for t in sensor.keyTimes where t >= start {
            let line = NSBezierPath()
            line.move(to: NSPoint(x: x(t), y: plot.minY))
            line.line(to: NSPoint(x: x(t), y: plot.maxY))
            line.lineWidth = 0.5
            line.stroke()
        }

        // 揺れ
        let path = NSBezierPath()
        for (index, sample) in visible.enumerated() {
            let point = NSPoint(x: x(sample.t), y: y(sample.magnitude))
            index == 0 ? path.move(to: point) : path.line(to: point)
        }
        NSColor.controlAccentColor.setStroke()
        path.lineWidth = 1
        path.stroke()
    }
}
