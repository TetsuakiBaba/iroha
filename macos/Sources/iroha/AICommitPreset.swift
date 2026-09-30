import AppKit
import Carbon.HIToolbox
import Foundation

/// AIバックエンドへの1回の依頼（system指示 + 入力テキスト）
struct AIRequest: Sendable {
    var instructions: String
    var userMessage: String

    /// プロンプトと入力テキストから依頼を組み立てる。
    /// プロンプトに {text} があればその位置へ差し込み、無ければ指示のあとに続けて渡す
    /// （AI変換プリセットと選択テキストプリセットで共通の規則）
    static func build(prompt: String, text: String) -> AIRequest {
        if prompt.contains(AICommitSettings.textPlaceholder) {
            return AIRequest(
                instructions: AICommitSettings.outputRule,
                userMessage: prompt.replacingOccurrences(
                    of: AICommitSettings.textPlaceholder, with: text))
        }
        return AIRequest(
            instructions: prompt + "\n" + AICommitSettings.outputRule, userMessage: text)
    }
}

/// 「AI変換」のショートカット（修飾キー+キー）。
/// 保存形式は選択テキストのショートカットと同じ "Option+Return" のような文字列で、`GlobalShortcut.parse` で解釈する
/// （空なら割り当てなし）。選択テキストと違いグローバルには登録せず、入力中に IME に届いたキーと照らし合わせる。
/// ⌃Return は macOS 15 以降、AppKit のアプリ（テキストエディット等）が右クリックメニューを出すキーとして
/// 入力メソッドに渡す前に使うので、入力しても効かないアプリがある（2026-09-30 実測。Slack・Teams では届く）
struct AICommitShortcut: Equatable {
    let rawValue: String

    var isEmpty: Bool { rawValue.trimmingCharacters(in: .whitespaces).isEmpty }

    /// 書式として解釈できるか（空は割り当てなしとして有効扱いにしない）
    var isValid: Bool { GlobalShortcut.isValid(rawValue) }

    /// 押されたキーがこのショートカットか。Return はテンキーの Enter でも一致させる
    func matches(keyCode: UInt16, modifierFlags: NSEvent.ModifierFlags) -> Bool {
        guard let parsed = GlobalShortcut.parse(rawValue) else { return false }
        let code = Int(keyCode)
        let keyMatches = Int(parsed.keyCode) == code
            || (Int(parsed.keyCode) == kVK_Return && code == kVK_ANSI_KeypadEnter)
        return keyMatches
            && Self.flags(carbonModifiers: parsed.carbonModifiers)
                == modifierFlags.intersection([.command, .control, .option, .shift])
    }

    /// 同じキーの組み合わせか（"Opt+Return" と "Option+Return" のような書き方の違いは同じとみなす）
    func isSameKey(as other: AICommitShortcut) -> Bool {
        guard let lhs = GlobalShortcut.parse(rawValue), let rhs = GlobalShortcut.parse(other.rawValue)
        else { return false }
        return lhs.keyCode == rhs.keyCode && lhs.carbonModifiers == rhs.carbonModifiers
    }

    /// ⌃Return（Shift なども付かない Control だけ）か。AppKit のアプリで効かないので設定画面で注意を出す
    var isControlReturn: Bool {
        guard let parsed = GlobalShortcut.parse(rawValue) else { return false }
        return Int(parsed.keyCode) == kVK_Return && parsed.carbonModifiers == UInt32(controlKey)
    }

    private static func flags(carbonModifiers: UInt32) -> NSEvent.ModifierFlags {
        var flags: NSEvent.ModifierFlags = []
        if carbonModifiers & UInt32(cmdKey) != 0 { flags.insert(.command) }
        if carbonModifiers & UInt32(controlKey) != 0 { flags.insert(.control) }
        if carbonModifiers & UInt32(optionKey) != 0 { flags.insert(.option) }
        if carbonModifiers & UInt32(shiftKey) != 0 { flags.insert(.shift) }
        return flags
    }

    /// 以前の保存形式（修飾キーの選択肢。`aiPreset{n}Shortcut`）を今の書式にする
    static func fromLegacy(_ legacy: String) -> String {
        switch legacy {
        case "control": return "Ctrl+Return"
        case "option": return "Option+Return"
        case "shift": return "Shift+Return"
        case "command": return "Cmd+Return"
        case "shift+control": return "Shift+Ctrl+Return"
        case "shift+option": return "Shift+Option+Return"
        case "shift+command": return "Shift+Cmd+Return"
        default: return ""  // "off" など
        }
    }
}

/// 「AI変換」の1つ分の設定。
///
/// 英訳もユーザ定義の変換も、AIに違うプロンプトを渡しているだけで仕組みは同じなので
/// 同じ形で3つ持つ（1つ目は既定で英訳のプロンプトが入っている）。
struct AICommitPreset: Identifiable, Equatable {
    var index: Int
    var name: String
    var prompt: String
    var shortcut: AICommitShortcut

    var id: Int { index }

    /// 表示名（空なら既定の名前）
    var displayName: String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? AICommitSettings.defaults[index].name : trimmed
    }

    /// 実際にAIへ渡すプロンプト（空なら既定のプロンプト）
    var effectivePrompt: String {
        let trimmed = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? AICommitSettings.defaults[index].prompt : trimmed
    }

    /// 未確定文字列をAIへの依頼に組み立てる（{text}差し込みの規則はAIRequest.buildを参照）
    func request(for text: String) -> AIRequest {
        AIRequest.build(prompt: effectivePrompt, text: text)
    }
}

/// AI変換まわりの設定（UserDefaults）。
enum AICommitSettings {

    /// 設定できるプリセットの数
    static let count = 3

    /// プロンプト内で入力テキストの位置を指定するための差し込み記号
    static let textPlaceholder = "{text}"

    /// 出力を1本のテキストに絞るための念押し（プロンプトの末尾に足す）
    static let outputRule =
        "出力は変換後のテキストのみ。説明・注釈・引用符・前置きを付けないこと。"

    /// 各プリセットの既定（名前・プロンプト・ショートカット）
    static let defaults: [(name: String, prompt: String, shortcut: String)] = [
        ("英訳", TranslationService.translateInstructions, "Option+Return"),
        ("敬語", "次の日本語を、意味を変えずに丁寧なビジネス文体に書き直してください。", ""),
        ("要約", "次の日本語を、要点を保ったまま短く言い換えてください。", ""),
    ]

    static func nameKey(_ index: Int) -> String { "aiPreset\(index)Name" }
    static func promptKey(_ index: Int) -> String { "aiPreset\(index)Prompt" }
    /// ショートカット（"Option+Return" 形式の文字列）
    static func hotkeyKey(_ index: Int) -> String { "aiPreset\(index)Hotkey" }
    /// 以前のショートカットの保存先（修飾キーの選択肢。`migrateHotkeysIfNeeded` で読み替える）
    static func legacyShortcutKey(_ index: Int) -> String { "aiPreset\(index)Shortcut" }

    static func preset(_ index: Int) -> AICommitPreset {
        let defaults = UserDefaults.standard
        return AICommitPreset(
            index: index,
            name: defaults.string(forKey: nameKey(index)) ?? Self.defaults[index].name,
            prompt: defaults.string(forKey: promptKey(index)) ?? Self.defaults[index].prompt,
            shortcut: AICommitShortcut(
                rawValue: defaults.string(forKey: hotkeyKey(index)) ?? Self.defaults[index].shortcut))
    }

    static var presets: [AICommitPreset] { (0..<count).map(preset) }

    /// 押されたキーに対応するプリセット（無ければnil。同じキーが複数にあれば番号の小さいほう）
    static func preset(matchingKeyCode keyCode: UInt16, modifierFlags: NSEvent.ModifierFlags) -> AICommitPreset? {
        presets.first { $0.shortcut.matches(keyCode: keyCode, modifierFlags: modifierFlags) }
    }

    // MARK: - 旧設定からの移行

    private static let migratedKey = "aiPresetsMigrated"

    /// 「英訳して確定」「AI変換して確定」が別設定だった頃の値をプリセットへ移す
    static func migrateIfNeeded() {
        let defaults = UserDefaults.standard
        guard !defaults.bool(forKey: migratedKey) else { return }
        defaults.set(true, forKey: migratedKey)

        if let old = defaults.string(forKey: "translateCommitModifier") {
            defaults.set(old, forKey: legacyShortcutKey(0))
        }
        if let old = defaults.string(forKey: "aiCommitModifier") {
            defaults.set(old, forKey: legacyShortcutKey(1))
        }
        if let old = defaults.string(forKey: "aiCommitPrompt"),
           !old.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            defaults.set(old, forKey: promptKey(1))
            defaults.set("AI変換", forKey: nameKey(1))
        }
    }

    /// 修飾キーの選択肢だった頃のショートカット（`aiPreset{n}Shortcut`）を、自由に書ける形式
    /// （`aiPreset{n}Hotkey`）へ読み替える。新しいキーが既にあるプリセットには触らない
    static func migrateHotkeysIfNeeded() {
        let defaults = UserDefaults.standard
        for index in 0..<count where defaults.string(forKey: hotkeyKey(index)) == nil {
            guard let legacy = defaults.string(forKey: legacyShortcutKey(index)) else { continue }
            defaults.set(AICommitShortcut.fromLegacy(legacy), forKey: hotkeyKey(index))
        }
    }
}
