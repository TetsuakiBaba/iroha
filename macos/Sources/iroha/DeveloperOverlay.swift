import Cocoa
import IrohaCore

/// デバッグ表示の設定（設定 > 情報 > 開発者向け）。開発者向けで既定OFF。
/// 開発者モード（`DeveloperModeSettings`）が ON のときだけ効く（OFF にしたら表示も止める）。
/// 他の Mac とは同期しない（`PreferencesSync.syncedKeys` に入れない）
enum DeveloperOverlaySettings {
    static let enabledKey = "developerOverlay"

    static var isEnabled: Bool {
        DeveloperModeSettings.isEnabled
            && UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? false
    }
}

/// 推論のたびに、かかった時間をカーソルの右下に小さく出す窓（デバッグ表示）。
///
/// 予測・打ち間違いの知らせを出す `CaretPanel` とは別の窓にする。`CaretPanel` は1枚を
/// 使い回すことで「訂正の小窓が出ている ⇒ Backspace で取り消せる」を保っているので、
/// そこに相乗りするとその約束が崩れる。こちらはカーソル行の下（右寄り）に出す。行の上に出すと
/// 書き終えた上の行が隠れて読み返せないため（2026-09-30 に右上から変更）。`CaretPanel`（行の下。
/// 画面の下端では行の上）と重なるときは `CaretPanel` のさらに外側へ逃がす。
///
/// 行は「左文脈」「かな漢字変換」「打ち間違いの訂正」の3本。左文脈は入力を始めたときに
/// アプリから読めたか（読めなければその理由と、代わりに使う文字列）を出す。
/// 変換と訂正はそれぞれ最後の1回を出す。
/// 時間では閉じない（読んでいる途中で消えないように）。中身は次の推論で入れ替わり、
/// 確定のあと次の入力を始めたとき・他の場所をクリックしたとき・アプリを切り替えたときに閉じる
/// （閉じると中身も捨てるので、1枚に出るのは常に同じ入力についての数字になる）。
/// キーボードフォーカスは取らず、マウスも透過する。メインスレッドから使う
final class DeveloperOverlay {
    static let shared = DeveloperOverlay()

    /// 変換要求の種類（辞書ラティスを通すか・何回変換するかが違うので、行の頭に出す）
    enum ConversionKind: String {
        case live = "ライブ変換"
        case segmenting = "文節に分ける"
        case resegmenting = "文節の区切り直し"
        case candidates = "候補ウィンドウ"
    }

    /// 打ち間違いの訂正を走らせたきっかけ
    enum TypoTrigger: String {
        case idle = "入力の休止"
        case conversion = "スペース"
    }

    /// 訂正の結果（`TypoNormalizer.correction` の戻りと、呼び出し側で捨てたかどうか）
    enum TypoOutcome: String {
        case corrected = "訂正した"
        case none = "訂正なし"
        case trailingInsertion = "末尾に足しただけなので捨てた"
    }

    private let panel: NSPanel
    private let label: NSTextField
    private let padding = NSEdgeInsets(top: 3, left: 6, bottom: 3, right: 6)
    private let caretGap: CGFloat = 2

    private var contextLine: String?
    private var conversionLine: String?
    private var typoLine: String?
    /// 前回かな漢字変換の行を出してから、結果を使わずに打ち切った変換の数
    private var cancelledConversions = 0
    private var lastCaretRect: NSRect?

    private init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 120, height: 20),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: true
        )
        panel.level = .popUpMenu
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.ignoresMouseEvents = true
        panel.hidesOnDeactivate = false
        panel.isReleasedWhenClosed = false
        panel.animationBehavior = .none

        let background = NSVisualEffectView()
        background.material = .popover
        background.blendingMode = .behindWindow
        background.state = .active
        background.wantsLayer = true
        background.layer?.cornerRadius = 4
        background.layer?.masksToBounds = true
        panel.contentView = background

        label = NSTextField(labelWithString: "")
        label.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize - 1, weight: .regular)
        label.textColor = .secondaryLabelColor
        label.maximumNumberOfLines = 0
        background.addSubview(label)
    }

    // MARK: - 行の組み立て

    /// 入力を始めたときに左文脈をアプリから読めたか。読めなかったときの `fallback` は
    /// 代わりに使う iroha 自身の確定文字列の蓄積（読めたときは nil）
    func reportContext(_ result: DocumentContextSettings.ReadResult, fallback: String?, near caretRect: NSRect?) {
        var line = "左文脈  "
        switch result {
        case .text(let text):
            line += "アプリから \(text.count)文字 \(Self.quoted(text))"
        case .atStart:
            line += "アプリから 0文字（カーソル位置が 0。前に文字があるならアプリが位置を返していない）"
        case .disabled:
            line += "アプリから読む設定が OFF"
        case .noCursor:
            line += "アプリから読めない（カーソル位置を返さない）"
        case .noText:
            line += "アプリから読めない（テキストを返さない）"
        }
        if let fallback {
            line += fallback.isEmpty
                ? " → 代わりの確定文字列もなし（文脈なし）"
                : " → 代わりに確定文字列 \(fallback.count)文字 \(Self.quoted(fallback))"
        }
        contextLine = line
        show(near: caretRect)
    }

    /// 末尾 `quotedLength` 文字を「」で囲む（窓の幅を抑える。長ければ先頭を … にする）
    private static func quoted(_ text: String) -> String {
        let quotedLength = 20
        return text.count > quotedLength ? "「…\(text.suffix(quotedLength))」" : "「\(text)」"
    }

    /// かな漢字変換 1 回ぶんを出す。取り消しの数はここで出して数え直す
    func reportConversion(
        _ kind: ConversionKind, total: Duration, timer: InferenceTimer.Snapshot, readingLength: Int,
        near caretRect: NSRect?
    ) {
        var parts = ["\(kind.rawValue)  全体 \(Self.format(total))"]
        parts.append(Self.neuralNetworkPart(timer, showsCount: true))
        parts.append("読み \(readingLength)文字")
        if timer.modelLoad > .zero {
            parts.append("モデル読み込み \(Self.format(timer.modelLoad))（全体に含む）")
        }
        if cancelledConversions > 0 {
            parts.append("取り消し \(cancelledConversions)")
        }
        cancelledConversions = 0
        conversionLine = parts.joined(separator: " ・ ")
        show(near: caretRect)
    }

    /// かな漢字変換を結果を使わずに打ち切った（次の行に数を添える）
    func noteCancelledConversion() {
        cancelledConversions += 1
    }

    /// 打ち間違いの訂正 1 回ぶんを出す
    func reportTypo(
        _ trigger: TypoTrigger, total: Duration, timer: InferenceTimer.Snapshot, readingLength: Int,
        outcome: TypoOutcome, near caretRect: NSRect?
    ) {
        var parts = ["打ち間違い（\(trigger.rawValue)）  全体 \(Self.format(total))"]
        parts.append(Self.neuralNetworkPart(timer, showsCount: false))
        parts.append("読み \(readingLength)文字")
        if timer.modelLoad > .zero {
            parts.append("モデル読み込み \(Self.format(timer.modelLoad))（全体に含む）")
        }
        parts.append(outcome.rawValue)
        typoLine = parts.joined(separator: " ・ ")
        show(near: caretRect)
    }

    private static func neuralNetworkPart(_ timer: InferenceTimer.Snapshot, showsCount: Bool) -> String {
        guard timer.neuralNetworkCalls > 0 else {
            // NN を通さずに返した。理由がわかれば添える（読み全体が学習・ユーザ辞書と一致した、など）
            let reasons = InferenceTimer.Shortcut.allCases.filter(timer.shortcuts.contains).map(\.label)
            return reasons.isEmpty ? "NN なし" : "NN なし（\(reasons.joined(separator: "・"))）"
        }
        let time = "NN \(format(timer.neuralNetwork))"
        return showsCount ? "\(time)（\(timer.neuralNetworkCalls)回）" : time
    }

    /// 100ms 未満は小数 1 桁、それ以上は整数の ms
    static func format(_ duration: Duration) -> String {
        let milliseconds = Double(duration.components.seconds) * 1000
            + Double(duration.components.attoseconds) / 1e15
        return milliseconds < 100
            ? String(format: "%.1fms", milliseconds)
            : String(format: "%.0fms", milliseconds)
    }

    // MARK: - 表示

    var isVisible: Bool { panel.isVisible }

    /// 未確定文字列が変わってカーソルが動いたときに位置だけ追う
    func follow(_ caretRect: NSRect?) {
        guard panel.isVisible, let caretRect else { return }
        lastCaretRect = caretRect
        layout()
    }

    /// `CaretPanel` が出た・消えたときに、重ならない位置へ置き直す
    func relayoutIfVisible() {
        guard panel.isVisible else { return }
        layout()
    }

    func hide() {
        contextLine = nil
        conversionLine = nil
        typoLine = nil
        cancelledConversions = 0
        guard panel.isVisible else { return }
        panel.orderOut(nil)
    }

    private func show(near caretRect: NSRect?) {
        // カーソル位置を教えてくれないアプリでは、前回の位置があればそこに出す
        if let caretRect { lastCaretRect = caretRect }
        guard lastCaretRect != nil else { return }
        layout()
        panel.orderFrontRegardless()
    }

    private func layout() {
        guard let caretRect = lastCaretRect else { return }
        label.stringValue = [contextLine, conversionLine, typoLine].compactMap { $0 }.joined(separator: "\n")
        label.sizeToFit()
        let width = padding.left + label.frame.width + padding.right
        let height = padding.top + label.frame.height + padding.bottom
        label.frame.origin = NSPoint(x: padding.left, y: padding.bottom)

        // カーソルの右下。`CaretPanel` と重なるならその下へ、画面の下に収まらなければ行の上へ
        // （行の上で `CaretPanel` と重なるならその上へ）
        var frame = NSRect(
            x: caretRect.maxX + caretGap, y: caretRect.minY - caretGap - height, width: width, height: height)
        if let other = CaretPanel.shared.visibleFrame, frame.intersects(other) {
            frame.origin.y = other.minY - caretGap - height
        }
        let screen = NSScreen.screens.first { $0.frame.contains(caretRect.origin) } ?? NSScreen.main
        if let visible = screen?.visibleFrame {
            if frame.minY < visible.minY {
                frame.origin.y = caretRect.maxY + caretGap
                if let other = CaretPanel.shared.visibleFrame, frame.intersects(other) {
                    frame.origin.y = other.maxY + caretGap
                }
            }
            frame.origin.x = min(max(frame.origin.x, visible.minX), max(visible.minX, visible.maxX - width))
        }
        panel.setFrame(frame, display: true)
    }
}

private extension InferenceTimer.Shortcut {
    var label: String {
        switch self {
        case .learning: return "学習"
        case .userDictionary: return "ユーザ辞書"
        case .cache: return "キャッシュ"
        }
    }
}
