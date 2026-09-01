import Foundation

/// マウス選択トリガーの出し方
enum SelectionTriggerMode: String, CaseIterable, Identifiable {
    case off            // マウストリガーなし（ショートカットのみ）
    case bubble         // 選択するとアイコンが出て、クリックでメニュー
    case immediateMenu  // 選択すると即メニュー
    case optionMenu     // ⌥を押しながら選択するとメニュー

    var id: String { rawValue }

    var label: String {
        switch self {
        case .off: return "オフ"
        case .bubble: return "アイコンを表示（クリックでメニュー）"
        case .immediateMenu: return "すぐにメニューを表示"
        case .optionMenu: return "⌥ optionを押しながら選択でメニュー"
        }
    }
}

/// 選択テキスト処理の1プリセット。
/// AI変換プリセット（AICommitPreset）とは独立した5枠で、グローバルショートカットを持つ
struct SelectionPreset: Identifiable, Equatable {
    var index: Int
    var name: String
    var prompt: String
    var hotkey: String  // "Ctrl+1" 形式。空なら割り当てなし
    var enabled: Bool

    var id: Int { index }

    var displayName: String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? "プリセット\(index + 1)" : trimmed
    }

    var effectivePrompt: String {
        let trimmed = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? SelectionSettings.defaults[index].prompt : trimmed
    }

    /// 選択テキストをAIへの依頼に組み立てる（{text}差し込みはAI変換プリセットと同じ規則）
    func request(for text: String) -> AIRequest {
        AIRequest.build(prompt: effectivePrompt, text: text)
    }
}

/// 選択テキスト処理まわりの設定（UserDefaults）
enum SelectionSettings {

    /// プリセットの枠数
    static let count = 5

    // 全体設定のキー
    static let enabledKey = "selectionEnabled"                    // 機能全体（既定OFF・オプトイン）
    static let triggerModeKey = "selectionTriggerMode"            // マウストリガーの出し方
    static let onDemandHotkeyKey = "selectionOnDemandHotkey"      // その場でプロンプト入力
    static let excludedBundleIdsKey = "selectionExcludedBundleIds"  // 除外アプリ（カンマ/改行区切り）

    static var isEnabled: Bool {
        UserDefaults.standard.bool(forKey: enabledKey)
    }

    static var triggerMode: SelectionTriggerMode {
        SelectionTriggerMode(
            rawValue: UserDefaults.standard.string(forKey: triggerModeKey) ?? "bubble"
        ) ?? .bubble
    }

    static var onDemandHotkey: String {
        UserDefaults.standard.string(forKey: onDemandHotkeyKey) ?? "Ctrl+0"
    }

    /// 除外バンドルIDの集合（小文字化して比較）
    static var excludedBundleIdentifiers: Set<String> {
        let raw = UserDefaults.standard.string(forKey: excludedBundleIdsKey) ?? ""
        return Set(
            raw.components(separatedBy: CharacterSet(charactersIn: ",\n"))
                .map { $0.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() }
                .filter { !$0.isEmpty }
        )
    }

    /// 各プリセットの既定。ショートカットは修飾キー+数字で覚えやすく揃える
    static let defaults: [(name: String, prompt: String, hotkey: String, enabled: Bool)] = [
        ("日英翻訳",
         "次のテキストを翻訳してください。日本語なら英語に、英語なら日本語に訳してください。",
         "Ctrl+1", true),
        ("校正",
         "次のテキストの誤字脱字と不自然な表現を直してください。意味と文体は変えないでください。",
         "Ctrl+2", true),
        ("敬語",
         "次の日本語を、意味を変えずに丁寧なビジネス文体に書き直してください。",
         "Ctrl+3", true),
        ("要約",
         "次のテキストを、要点を保ったまま短く言い換えてください。",
         "Ctrl+4", true),
        ("カスタム", "", "", false),
    ]

    static func nameKey(_ index: Int) -> String { "selPreset\(index)Name" }
    static func promptKey(_ index: Int) -> String { "selPreset\(index)Prompt" }
    static func hotkeyKey(_ index: Int) -> String { "selPreset\(index)Hotkey" }
    static func enabledObjectKey(_ index: Int) -> String { "selPreset\(index)Enabled" }

    static func preset(_ index: Int) -> SelectionPreset {
        let store = UserDefaults.standard
        let def = defaults[index]
        return SelectionPreset(
            index: index,
            name: store.string(forKey: nameKey(index)) ?? def.name,
            prompt: store.string(forKey: promptKey(index)) ?? def.prompt,
            hotkey: store.string(forKey: hotkeyKey(index)) ?? def.hotkey,
            enabled: (store.object(forKey: enabledObjectKey(index)) as? Bool) ?? def.enabled
        )
    }

    static var presets: [SelectionPreset] { (0..<count).map(preset) }

    static var enabledPresets: [SelectionPreset] { presets.filter(\.enabled) }
}
