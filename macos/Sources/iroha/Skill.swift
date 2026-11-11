import Combine
import Foundation
import IrohaCore

/// スキル（青空写経のコードで解除する機能。2026-11-11、v1.0.0 から）。
///
/// 設定項目は 2 つに分ける:
/// - 基本: インストールした直後から設定画面に出る
/// - スキル: コード 1 つで、ユーザが選んだ 1 つを解除する。コードが 7 つ揃うとすべて解除される（`SkillState`）
///
/// **新しい設定項目を足すときは、基本かスキルかを決めてから足す。**スキルにするなら:
/// 1. ここに case を足し、`title`・`summary`・`location` を書く（rawValue は skills.json に残るので後から変えない）
/// 2. 設定画面では `skills.has(.x)` の中に項目を置く（`SkillObserver`）
/// 3. 機能の側でも `Skill.x.isUnlocked` を確かめる。設定画面で隠すだけでは、既定のショートカットや
///    別の Mac から同期された設定で動いてしまう（各設定の `isEnabled` / `isActive` に入れてある）
///
/// 目的はユーザが進み具合を楽しむことで、厳密に制限することではない（照合の方法は `SkillCode`）
enum Skill: String, CaseIterable, Identifiable {
    case prediction
    case rewriteRules
    case aiConversion
    case selectionEdit
    case characterCount
    case delimitedAlphabet
    case tapInput
    case aiFix
    case documentNotation
    case training
    case typoTuning
    case modelSelection
    case inferenceOverlay

    var id: String { rawValue }

    var title: String {
        switch self {
        case .prediction: return "予測変換"
        case .rewriteRules: return "変換ルール"
        case .aiConversion: return "AI変換"
        case .selectionEdit: return "選択テキストのAI編集"
        case .characterCount: return "選択した文字数の表示"
        case .delimitedAlphabet: return "区切り記号で英字を入力"
        case .tapInput: return "タップ入力"
        case .aiFix: return "AIで直す"
        case .documentNotation: return "自分の原稿に合わせる"
        case .training: return "自分の入力で追加学習"
        case .typoTuning: return "打ち間違いの訂正の調整"
        case .modelSelection: return "推論エンジンとモデルの指定"
        case .inferenceOverlay: return "推論の時間の表示"
        }
    }

    /// スキルの一覧に出す 1 行の説明
    var summary: String {
        switch self {
        case .prediction: return "これまでに確定した語から、読みの先頭が一致する語を入力中に出します。"
        case .rewriteRules: return "「きょう」で今日の日付のように、読みと出力の組を登録して候補に出します。"
        case .aiConversion: return "入力中の文字列を、修飾キー+Returnで AI に渡して書き換えます。"
        case .selectionEdit: return "どのアプリでも、選択したテキストを AI で処理して置き換えます。"
        case .characterCount: return "マウスで選択した文字数を、選択範囲の近くに出します。"
        case .delimitedAlphabet: return "かな入力のまま、記号で囲んだ部分を打ったとおりの英字にします。"
        case .tapInput: return "⌘ を押したまま本体を指で 2 回叩いて、決めたキーショートカットを送ります。"
        case .aiFix: return "誤変換の疑いがあるときに「AIで直す」を出し、押すと AI の案に差し替えます。"
        case .documentNotation: return "自分の原稿（PDF・Word・テキスト）の表記と用語に、変換を合わせます。"
        case .training: return "確定した変換を記録し、その記録で変換モデルを追加学習します。"
        case .typoTuning: return "訂正するまでの休止時間・最低文字数・確信の強さと、訂正モデルの入れ替え。"
        case .modelSelection: return "推論エンジン（llama.cpp ／ MLX）と、使うモデルファイルを選びます。"
        case .inferenceOverlay: return "変換にかかった時間と、変換に使った左文脈をカーソルの右下に出します。"
        }
    }

    /// 解除したあと、設定画面のどこに項目が出るか
    var location: String {
        switch self {
        case .prediction, .aiConversion, .delimitedAlphabet, .tapInput, .aiFix, .typoTuning: return "入力"
        case .rewriteRules, .documentNotation: return "辞書・学習"
        case .selectionEdit, .characterCount: return "選択テキスト"
        case .training: return "辞書・学習（変換記録）とモデル"
        case .modelSelection: return "モデル"
        case .inferenceOverlay: return "情報"
        }
    }

    /// この Mac で使えないスキルなら、その理由（使えるなら nil）。使えないスキルは選べないようにする
    var unavailableReason: String? {
        switch self {
        case .tapInput where !KeystrokeAccelerometer.isSensorPresent:
            return "この Mac には加速度センサがないので使えません"
        default:
            return nil
        }
    }

    /// 解除されているか（機能の側の判定に使う。キーを打つたびに呼んでよい）
    var isUnlocked: Bool { SkillStore.shared.isUnlocked(rawValue) }
}

/// 設定画面がスキルの状態の変化を受け取るための入れ物（`SkillStore.didChangeNotification` を SwiftUI に渡す）
@MainActor
final class SkillObserver: ObservableObject {
    static let shared = SkillObserver()

    @Published private(set) var state: SkillState

    private var observer: NSObjectProtocol?

    private init() {
        state = SkillStore.shared.state
        observer = NotificationCenter.default.addObserver(
            forName: SkillStore.didChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.state = SkillStore.shared.state }
        }
    }

    func has(_ skill: Skill) -> Bool { state.isUnlocked(skill.rawValue) }

    /// コードが 7 つ揃っている（すべてのスキルと、単独のスキルにしていない細かな調整が出る）
    var allUnlocked: Bool { state.isAllUnlocked }

    /// AI サービスの設定を使うスキルのどれかが解除されている
    var usesAIService: Bool { has(.aiConversion) || has(.selectionEdit) || has(.aiFix) }
}
