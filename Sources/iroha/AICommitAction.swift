import AppKit
import Foundation

/// AIバックエンドへの1回の依頼（system指示 + 入力テキスト）
struct AIRequest: Sendable {
    var instructions: String
    var userMessage: String
}

/// 修飾キー+Enterで走る「AIで処理して確定」の種類。
/// どちらも設定の「AIサービス」（Apple Intelligence / Ollama / LM Studio）を使う
enum AICommitAction: Sendable {
    case translate                // 英訳して確定
    case custom(prompt: String)   // AI変換して確定（プロンプトはユーザが設定）

    /// 出力を1本のテキストに絞るための念押し（プロンプトの末尾に足す）
    private static let outputRule =
        "出力は変換後のテキストのみ。説明・注釈・引用符・前置きを付けないこと。"

    func request(for text: String) -> AIRequest {
        switch self {
        case .translate:
            return AIRequest(
                instructions: TranslationService.translateInstructions, userMessage: text)
        case .custom(let prompt):
            let prompt = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !prompt.isEmpty else {
                return AIRequest(instructions: Self.outputRule, userMessage: text)
            }
            // {text} があればそこへ入力を差し込む。無ければ指示のあとに入力を続ける
            if prompt.contains(AICommitSettings.textPlaceholder) {
                return AIRequest(
                    instructions: Self.outputRule,
                    userMessage: prompt.replacingOccurrences(
                        of: AICommitSettings.textPlaceholder, with: text))
            }
            return AIRequest(
                instructions: prompt + "\n" + Self.outputRule, userMessage: text)
        }
    }
}

/// AI確定まわりの設定（UserDefaults）。
enum AICommitSettings {

    /// プロンプト内で入力テキストの位置を指定するための差し込み記号
    static let textPlaceholder = "{text}"

    /// 英訳して確定のショートカット（既定は control + Return）
    static let translateModifierKey = "translateCommitModifier"
    /// AI変換して確定のショートカット（既定はオフ）
    static let customModifierKey = "aiCommitModifier"
    /// AI変換して確定のプロンプト
    static let customPromptKey = "aiCommitPrompt"

    static let defaultPrompt = "次の日本語を、意味を変えずに丁寧なビジネス文体に書き直してください。"

    static var customPrompt: String {
        let stored = UserDefaults.standard.string(forKey: customPromptKey) ?? ""
        return stored.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            ? defaultPrompt : stored
    }

    /// 設定値（"control" / "option" / "shift" / "command" / "off"）を修飾キーに変換する
    static func modifier(forKey key: String, fallback: String) -> NSEvent.ModifierFlags? {
        switch UserDefaults.standard.string(forKey: key) ?? fallback {
        case "control": return .control
        case "option": return .option
        case "shift": return .shift
        case "command": return .command
        default: return nil  // "off"
        }
    }
}
