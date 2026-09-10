import Foundation
import IOKit
import IOKit.hidsystem

/// Caps Lock キーで日本語 ⇄ 英字の入力モードを切り替える設定（ことえりの「Caps Lockの動作:
/// オンの時「英字」を入力」に相当）。
///
/// ことえりのこの機能はことえり自身のプロセスが処理しているため、いろはを選んでいる間は働かない。
/// いろはでは `flagsChanged` で Caps Lock の押下を受け取ってモードを切り替え、直後に Caps Lock の
/// 状態を OFF に戻す（LED を点けず、英字が大文字にならないようにする。macOS の「Caps Lockキーで
/// ラテン文字系入力ソースと切り替える」と同じ体感）
enum CapsLockSettings {

    static let enabledKey = "capsLockSwitchesMode"

    /// 既定ON（ことえりの既定に合わせる）
    static var isEnabled: Bool {
        UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true
    }

    /// Caps Lock の状態を OFF にする（IOKit HID）。失敗しても何もしない
    static func turnCapsLockOff() {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IOHIDSystem"), &iterator) == KERN_SUCCESS
        else { return }
        let service = IOIteratorNext(iterator)
        IOObjectRelease(iterator)
        guard service != 0 else { return }
        defer { IOObjectRelease(service) }
        var connect: io_connect_t = 0
        guard IOServiceOpen(service, mach_task_self_, UInt32(kIOHIDParamConnectType), &connect) == KERN_SUCCESS
        else { return }
        defer { IOServiceClose(connect) }
        IOHIDSetModifierLockState(connect, Int32(kIOHIDCapsLockState), false)
    }
}
