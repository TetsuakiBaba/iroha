import AppKit
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

/// 「AI変換して確定」に割り当てられる修飾キー+Return。
/// rawValueがUserDefaultsに保存される（旧設定の "control" 等とも互換）
enum AICommitShortcut: String, CaseIterable, Identifiable {
    case off
    case control
    case option
    case shift
    case command
    case shiftControl = "shift+control"
    case shiftOption = "shift+option"
    case shiftCommand = "shift+command"

    var id: String { rawValue }

    /// nilなら割り当てなし
    var flags: NSEvent.ModifierFlags? {
        switch self {
        case .off: return nil
        case .control: return .control
        case .option: return .option
        case .shift: return .shift
        case .command: return .command
        case .shiftControl: return [.shift, .control]
        case .shiftOption: return [.shift, .option]
        case .shiftCommand: return [.shift, .command]
        }
    }

    var label: String {
        switch self {
        case .off: return "オフ"
        case .control: return "⌃ control + Return"
        case .option: return "⌥ option + Return"
        case .shift: return "⇧ shift + Return"
        case .command: return "⌘ command + Return"
        case .shiftControl: return "⇧⌃ shift + control + Return"
        case .shiftOption: return "⇧⌥ shift + option + Return"
        case .shiftCommand: return "⇧⌘ shift + command + Return"
        }
    }
}

/// 「AI変換して確定」の1つ分の設定。
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

/// AI確定まわりの設定（UserDefaults）。
enum AICommitSettings {

    /// 設定できるプリセットの数
    static let count = 3

    /// プロンプト内で入力テキストの位置を指定するための差し込み記号
    static let textPlaceholder = "{text}"

    /// 出力を1本のテキストに絞るための念押し（プロンプトの末尾に足す）
    static let outputRule =
        "出力は変換後のテキストのみ。説明・注釈・引用符・前置きを付けないこと。"

    /// 各プリセットの既定（名前・プロンプト・ショートカット）
    static let defaults: [(name: String, prompt: String, shortcut: AICommitShortcut)] = [
        ("英訳", TranslationService.translateInstructions, .control),
        ("敬語", "次の日本語を、意味を変えずに丁寧なビジネス文体に書き直してください。", .off),
        ("要約", "次の日本語を、要点を保ったまま短く言い換えてください。", .off),
    ]

    static func nameKey(_ index: Int) -> String { "aiPreset\(index)Name" }
    static func promptKey(_ index: Int) -> String { "aiPreset\(index)Prompt" }
    static func shortcutKey(_ index: Int) -> String { "aiPreset\(index)Shortcut" }

    static func preset(_ index: Int) -> AICommitPreset {
        let defaults = UserDefaults.standard
        return AICommitPreset(
            index: index,
            name: defaults.string(forKey: nameKey(index)) ?? Self.defaults[index].name,
            prompt: defaults.string(forKey: promptKey(index)) ?? Self.defaults[index].prompt,
            shortcut: AICommitShortcut(
                rawValue: defaults.string(forKey: shortcutKey(index)) ?? "")
                ?? Self.defaults[index].shortcut)
    }

    static var presets: [AICommitPreset] { (0..<count).map(preset) }

    /// 押された修飾キーに対応するプリセット（無ければnil）
    static func preset(matching flags: NSEvent.ModifierFlags) -> AICommitPreset? {
        guard !flags.isEmpty else { return nil }
        return presets.first { $0.shortcut.flags == flags }
    }

    // MARK: - 旧設定からの移行

    private static let migratedKey = "aiPresetsMigrated"

    /// 「英訳して確定」「AI変換して確定」が別設定だった頃の値をプリセットへ移す
    static func migrateIfNeeded() {
        let defaults = UserDefaults.standard
        guard !defaults.bool(forKey: migratedKey) else { return }
        defaults.set(true, forKey: migratedKey)

        if let old = defaults.string(forKey: "translateCommitModifier") {
            defaults.set(old, forKey: shortcutKey(0))
        }
        if let old = defaults.string(forKey: "aiCommitModifier") {
            defaults.set(old, forKey: shortcutKey(1))
        }
        if let old = defaults.string(forKey: "aiCommitPrompt"),
           !old.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            defaults.set(old, forKey: promptKey(1))
            defaults.set("AI変換", forKey: nameKey(1))
        }
    }
}
