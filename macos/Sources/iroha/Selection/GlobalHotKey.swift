import Carbon.HIToolbox
import Foundation

/// "Ctrl+1" のような文字列をCarbonのキーコード+修飾キーへ変換する。
/// 選択テキスト処理のグローバルショートカットの保存形式（UserDefaults）でもある
struct GlobalShortcut: Hashable {
    let rawValue: String
    let keyCode: UInt32
    let carbonModifiers: UInt32

    static func parse(_ rawValue: String) -> GlobalShortcut? {
        let parts = rawValue
            .split(separator: "+")
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }

        guard parts.count >= 2, let keyToken = parts.last else { return nil }

        var modifiers: UInt32 = 0
        for token in parts.dropLast() {
            switch token.lowercased() {
            case "cmd", "command", "⌘":
                modifiers |= UInt32(cmdKey)
            case "ctrl", "control", "⌃":
                modifiers |= UInt32(controlKey)
            case "shift", "⇧":
                modifiers |= UInt32(shiftKey)
            case "alt", "option", "opt", "⌥":
                modifiers |= UInt32(optionKey)
            default:
                return nil
            }
        }

        guard let keyCode = keyCodeMap[keyToken.lowercased()] else { return nil }
        return GlobalShortcut(rawValue: rawValue, keyCode: keyCode, carbonModifiers: modifiers)
    }

    static func isValid(_ rawValue: String) -> Bool {
        parse(rawValue) != nil
    }

    /// ANSI配列のキーコード表（英数・記号・機能キー）
    private static let keyCodeMap: [String: UInt32] = [
        "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9,
        "b": 11, "q": 12, "w": 13, "e": 14, "r": 15, "y": 16, "t": 17, "1": 18, "2": 19,
        "3": 20, "4": 21, "6": 22, "5": 23, "=": 24, "9": 25, "7": 26, "-": 27, "8": 28,
        "0": 29, "]": 30, "o": 31, "u": 32, "[": 33, "i": 34, "p": 35, "l": 37, "j": 38,
        "'": 39, "k": 40, ";": 41, "\\": 42, ",": 43, "/": 44, "n": 45, "m": 46, ".": 47,
        "`": 50, "tab": 48, "space": 49, "enter": 36, "return": 36, "escape": 53, "esc": 53,
        "delete": 51, "backspace": 51, "up": 126, "down": 125, "left": 123, "right": 124,
        "home": 115, "end": 119, "pageup": 116, "pagedown": 121, "insert": 114,
        "f1": 122, "f2": 120, "f3": 99, "f4": 118, "f5": 96, "f6": 97, "f7": 98, "f8": 100,
        "f9": 101, "f10": 109, "f11": 103, "f12": 111,
    ]
}

/// Carbonのグローバルホットキー登録。IMEプロセスが生きている間、
/// どのアプリがフロントでも発火する（GenGoのHotKeyCenterを移植）
final class HotKeyCenter {
    private struct Registration {
        let hotKeyRef: EventHotKeyRef
        let action: () -> Void
    }

    private let signature: OSType = 0x49524F48  // "IROH"
    private var registrations: [UInt32: Registration] = [:]
    private var nextIdentifier: UInt32 = 1
    private var eventHandler: EventHandlerRef?

    init() {
        installEventHandler()
    }

    deinit {
        unregisterAll()
        if let eventHandler {
            RemoveEventHandler(eventHandler)
        }
    }

    func unregisterAll() {
        for registration in registrations.values {
            UnregisterEventHotKey(registration.hotKeyRef)
        }
        registrations.removeAll()
    }

    @discardableResult
    func register(shortcut rawShortcut: String, action: @escaping () -> Void) -> Bool {
        guard let shortcut = GlobalShortcut.parse(rawShortcut) else { return false }

        let hotKeyID = EventHotKeyID(signature: signature, id: nextIdentifier)
        var hotKeyRef: EventHotKeyRef?
        let status = RegisterEventHotKey(
            shortcut.keyCode,
            shortcut.carbonModifiers,
            hotKeyID,
            GetApplicationEventTarget(),
            0,
            &hotKeyRef
        )
        guard status == noErr, let hotKeyRef else { return false }

        registrations[nextIdentifier] = Registration(hotKeyRef: hotKeyRef, action: action)
        nextIdentifier += 1
        return true
    }

    private func installEventHandler() {
        var eventType = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed)
        )
        InstallEventHandler(
            GetApplicationEventTarget(),
            { _, eventRef, userData in
                guard let userData, let eventRef else { return noErr }
                let center = Unmanaged<HotKeyCenter>.fromOpaque(userData).takeUnretainedValue()
                return center.handleHotKeyEvent(eventRef)
            },
            1,
            &eventType,
            UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()),
            &eventHandler
        )
    }

    private func handleHotKeyEvent(_ eventRef: EventRef) -> OSStatus {
        var hotKeyID = EventHotKeyID()
        let status = GetEventParameter(
            eventRef,
            EventParamName(kEventParamDirectObject),
            EventParamType(typeEventHotKeyID),
            nil,
            MemoryLayout<EventHotKeyID>.size,
            nil,
            &hotKeyID
        )
        guard status == noErr, let registration = registrations[hotKeyID.id] else { return noErr }
        registration.action()
        return noErr
    }
}
