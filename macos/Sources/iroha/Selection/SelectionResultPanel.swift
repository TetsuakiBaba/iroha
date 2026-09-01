import AppKit
import SwiftUI

/// 選択テキスト処理パネルの表示状態
@MainActor
final class SelectionPanelModel: ObservableObject {
    enum Phase {
        case promptInput  // オンデマンド: プロンプト入力待ち
        case processing   // AI実行中（ストリーミング表示）
        case done         // 完了（Returnで適用できる）
        case notice       // 短い通知だけ（自動で消える）
    }

    @Published var phase: Phase = .processing
    @Published var title = ""
    @Published var sourceText = ""
    @Published var outputText = ""
    @Published var promptText = ""
    @Published var notice: String?
    /// true: 選択なしのテキスト生成（適用はカーソル位置への挿入になる）
    @Published var isGeneration = false
}

/// AIの結果を確認するフローティングパネル。
/// .nonactivatingPanel なので対象アプリを非アクティブにせずキー入力を受けられる
@MainActor
final class SelectionPanelController: NSWindowController {
    let model = SelectionPanelModel()

    var onSubmitPrompt: (() -> Void)?
    var onApply: (() -> Void)?
    /// 完了状態で⌘C（結果をコピー）。falseを返すとテキスト欄の通常コピーに流す
    var onCopyResult: (() -> Bool)?
    var onClose: (() -> Void)?

    private var autoDismissTask: Task<Void, Never>?

    init() {
        let panel = SelectionKeyPanel(
            contentRect: NSRect(x: 0, y: 0, width: 460, height: 240),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        panel.level = .popUpMenu
        panel.collectionBehavior = [.canJoinAllSpaces, .transient, .ignoresCycle]
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = false
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.isMovableByWindowBackground = true
        panel.isReleasedWhenClosed = false

        super.init(window: panel)

        panel.onReturnKey = { [weak self] in
            guard let self else { return }
            switch self.model.phase {
            case .done: self.onApply?()
            case .promptInput: self.onSubmitPrompt?()
            case .processing, .notice: break
            }
        }
        panel.onEscape = { [weak self] in self?.onClose?() }
        panel.onCopyKey = { [weak self] in self?.onCopyResult?() ?? false }

        panel.contentView = NSHostingView(
            rootView: SelectionPanelView(
                model: model,
                onSubmit: { [weak self] in self?.onSubmitPrompt?() },
                onApply: { [weak self] in self?.onApply?() },
                onCopy: { [weak self] in _ = self?.onCopyResult?() },
                onClose: { [weak self] in self?.onClose?() }
            )
        )
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    /// マウス位置の近くに表示する（画面内にクランプ）
    func present(near location: NSPoint) {
        guard let window else { return }
        autoDismissTask?.cancel()

        let size = contentSize(for: model.phase)
        window.setContentSize(size)

        let screen = NSScreen.screens.first { NSMouseInRect(location, $0.frame, false) } ?? NSScreen.main
        let visibleFrame = screen?.visibleFrame ?? .zero
        var origin = NSPoint(x: location.x + 16, y: location.y - size.height - 16)
        origin.x = min(max(origin.x, visibleFrame.minX + 8), visibleFrame.maxX - size.width - 8)
        origin.y = min(max(origin.y, visibleFrame.minY + 8), visibleFrame.maxY - size.height - 8)
        window.setFrameOrigin(origin)

        if model.phase == .notice {
            // 通知はフォーカスを奪わずに出すだけ
            window.orderFrontRegardless()
        } else {
            window.makeKeyAndOrderFront(nil)
            window.orderFrontRegardless()
        }

        if model.phase == .notice {
            autoDismissTask = Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: 1_800_000_000)
                guard !Task.isCancelled else { return }
                self?.dismiss()
            }
        }
    }

    /// 内容に合わせてサイズを更新する（左上を固定。ストリーミング中も呼ばれる）
    func refreshSize() {
        guard let window, window.isVisible else { return }
        let size = contentSize(for: model.phase)
        let frame = window.frame
        guard abs(frame.height - size.height) > 0.5 || abs(frame.width - size.width) > 0.5 else { return }
        var target = NSRect(
            x: frame.minX, y: frame.maxY - size.height, width: size.width, height: size.height)
        // 下や右に伸びて画面外へ出ないようにクランプ
        if let visibleFrame = (window.screen ?? NSScreen.main)?.visibleFrame {
            target.origin.x = min(max(target.minX, visibleFrame.minX + 8), visibleFrame.maxX - size.width - 8)
            target.origin.y = min(max(target.minY, visibleFrame.minY + 8), visibleFrame.maxY - size.height - 8)
        }
        window.setFrame(target, display: true)
    }

    func dismiss() {
        autoDismissTask?.cancel()
        autoDismissTask = nil
        window?.orderOut(nil)
    }

    var isVisible: Bool { window?.isVisible ?? false }

    private func contentSize(for phase: SelectionPanelModel.Phase) -> NSSize {
        switch phase {
        case .promptInput: return NSSize(width: 460, height: model.sourceText.isEmpty ? 120 : 168)
        case .processing, .done: return outputPanelSize()
        case .notice: return NSSize(width: 300, height: 56)
        }
    }

    /// 出力テキストを実測して、内容に見合ったサイズを返す。
    /// 短い結果は小さく、長い結果は上限まで広げてスクロールに任せる
    private func outputPanelSize() -> NSSize {
        let minWidth: CGFloat = 300   // フッタのボタン類が収まる幅
        let maxWidth: CGFloat = 460
        let maxHeight: CGFloat = 340
        // テキスト以外の固定分: 外周パディング12×2 + テキスト欄の内側8×2
        let chromeWidth: CGFloat = 40
        // ヘッダ + フッタ + 縦の余白ぶん
        let chromeHeight: CGFloat = 104

        let font = NSFont.preferredFont(forTextStyle: .body)
        let text = model.outputText.isEmpty ? "　" : model.outputText
        let attributed = NSAttributedString(string: text, attributes: [.font: font])
        let unbounded = NSSize(
            width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        let options: NSString.DrawingOptions = [.usesLineFragmentOrigin, .usesFontLeading]

        // 幅: 最長行に合わせて縮める（折り返しが必要なら上限のまま）
        let naturalRect = attributed.boundingRect(with: unbounded, options: options)
        let width = min(maxWidth, max(minWidth, ceil(naturalRect.width) + chromeWidth))

        // 高さ: その幅で折り返したときの行数ぶん
        let wrappedRect = attributed.boundingRect(
            with: NSSize(width: width - chromeWidth, height: .greatestFiniteMagnitude),
            options: options)
        let textHeight = max(ceil(wrappedRect.height), ceil(font.boundingRectForFont.height))
        let height = min(maxHeight, textHeight + chromeHeight)
        return NSSize(width: width, height: height)
    }
}

/// Return / Esc / ⌘キーをパネル側で扱うNSPanel。
/// IMEプロセスにはメインメニューが無いため、テキスト欄の⌘V等もここで配線する
private final class SelectionKeyPanel: NSPanel {
    var onReturnKey: (() -> Void)?
    var onEscape: (() -> Void)?
    var onCopyKey: (() -> Bool)?

    override var canBecomeKey: Bool { true }

    override func keyDown(with event: NSEvent) {
        switch Int(event.keyCode) {
        case 36, 76:  // Return / Enter
            onReturnKey?()
        case 53:  // Esc
            onEscape?()
        default:
            super.keyDown(with: event)
        }
    }

    override func cancelOperation(_ sender: Any?) {
        onEscape?()
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection([.command, .control, .option])
        guard modifiers == .command else { return super.performKeyEquivalent(with: event) }
        switch event.charactersIgnoringModifiers {
        case "c":
            if onCopyKey?() == true { return true }
            return NSApp.sendAction(#selector(NSText.copy(_:)), to: nil, from: self)
        case "v":
            return NSApp.sendAction(#selector(NSText.paste(_:)), to: nil, from: self)
        case "x":
            return NSApp.sendAction(#selector(NSText.cut(_:)), to: nil, from: self)
        case "a":
            return NSApp.sendAction(#selector(NSText.selectAll(_:)), to: nil, from: self)
        case "z":
            return NSApp.sendAction(Selector(("undo:")), to: nil, from: self)
        default:
            return super.performKeyEquivalent(with: event)
        }
    }
}

// MARK: - SwiftUI

private struct SelectionPanelView: View {
    @ObservedObject var model: SelectionPanelModel
    @FocusState private var promptFocused: Bool
    let onSubmit: () -> Void
    let onApply: () -> Void
    let onCopy: () -> Void
    let onClose: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if model.phase == .notice {
                noticeBody
            } else {
                header
                content
                footer
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(Color(nsColor: .windowBackgroundColor))
                .overlay(
                    RoundedRectangle(cornerRadius: 12)
                        .stroke(Color(nsColor: .separatorColor), lineWidth: 1)
                )
        )
    }

    private var noticeBody: some View {
        HStack(spacing: 8) {
            Image(systemName: "info.circle")
                .foregroundStyle(.secondary)
            Text(model.notice ?? "")
                .font(.callout)
            Spacer(minLength: 0)
        }
        .frame(maxHeight: .infinity)
    }

    private var header: some View {
        HStack(spacing: 6) {
            Image(systemName: "sparkles")
                .foregroundStyle(Color.accentColor)
            Text(model.title)
                .font(.headline)
            if model.phase == .processing {
                ProgressView()
                    .controlSize(.small)
                    .padding(.leading, 2)
            }
            Spacer()
            Button(action: onClose) {
                Image(systemName: "xmark.circle.fill")
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
        }
    }

    @ViewBuilder
    private var content: some View {
        switch model.phase {
        case .promptInput:
            if !model.sourceText.isEmpty {
                Text(model.sourceText)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
                    .background(
                        RoundedRectangle(cornerRadius: 6)
                            .fill(Color(nsColor: .textBackgroundColor)))
            }
            TextField(
                model.isGeneration
                    ? "作成してほしいテキスト（例: 会議欠席の連絡メール）"
                    : "やってほしいこと（例: 箇条書きにして）",
                text: $model.promptText
            )
            .textFieldStyle(.roundedBorder)
            .focused($promptFocused)
            .onAppear { promptFocused = true }
            .onSubmit(onSubmit)

        case .processing, .done:
            ScrollViewReader { proxy in
                ScrollView {
                    Text(model.outputText.isEmpty ? " " : model.outputText)
                        .font(.body)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .id("output")
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding(8)
                .background(
                    RoundedRectangle(cornerRadius: 6)
                        .fill(Color(nsColor: .textBackgroundColor)))
                .onChange(of: model.outputText) {
                    proxy.scrollTo("output", anchor: .bottom)
                }
            }
            if let notice = model.notice {
                Text(notice)
                    .font(.caption)
                    .foregroundStyle(.red)
            }

        case .notice:
            EmptyView()
        }
    }

    @ViewBuilder
    private var footer: some View {
        HStack(spacing: 8) {
            switch model.phase {
            case .promptInput:
                Spacer()
                Text("⏎ 実行   esc 閉じる")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            case .processing:
                Spacer()
                Text("esc キャンセル")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            case .done:
                Button(model.isGeneration ? "挿入 ⏎" : "置換 ⏎", action: onApply)
                    .keyboardShortcut(.defaultAction)
                    .disabled(model.outputText.isEmpty)
                Button("コピー ⌘C", action: onCopy)
                    .disabled(model.outputText.isEmpty)
                Spacer()
                Text("esc 閉じる")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            case .notice:
                EmptyView()
            }
        }
    }
}
