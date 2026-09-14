import AppKit
import SwiftUI

/// 選択メニューでの選択肢
enum SelectionMenuChoice {
    case onDemand
    case preset(index: Int)
}

/// マウス選択の直後に選択範囲の近くへ出す丸ボタン（GenGoのバブルを移植）。
/// クリックするとプリセットメニューを開く。
/// 選択した文字数の表示（設定でON）が有効なときは「12文字」のラベルを添えたピル型になり、
/// AI編集がOFFなら文字数だけを出す（クリックしても何もしない）
@MainActor
final class SelectionBubbleController: NSWindowController {
    private var dismissTask: Task<Void, Never>?
    private var action: (() -> Void)?
    private let hostingView: NSHostingView<SelectionBubbleView>

    init() {
        let panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 38, height: 38),
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
        panel.isMovable = false
        panel.ignoresMouseEvents = false
        panel.becomesKeyOnlyIfNeeded = true
        panel.isReleasedWhenClosed = false

        hostingView = NSHostingView(rootView: SelectionBubbleView(label: nil, showsIcon: true, action: nil))
        super.init(window: panel)
        panel.contentView = hostingView
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    /// `label` は文字数などの添え書き（nilなら丸いアイコンだけ）。`action` がnilならクリックできない表示専用。
    /// 両方nilなら何も出さない
    func present(at mouseLocation: NSPoint, label: String? = nil, action: (() -> Void)?) {
        guard let window, label != nil || action != nil else { return }

        dismissTask?.cancel()
        self.action = action

        hostingView.rootView = SelectionBubbleView(
            label: label,
            showsIcon: action != nil,
            action: action == nil ? nil : { [weak self] in self?.performAction() }
        )
        let size = hostingView.fittingSize
        window.setContentSize(size)
        let screen = NSScreen.screens.first { NSMouseInRect(mouseLocation, $0.frame, false) } ?? NSScreen.main
        let visibleFrame = screen?.visibleFrame ?? .zero
        var origin = NSPoint(x: mouseLocation.x + 10, y: mouseLocation.y - size.height - 10)
        origin.x = min(max(origin.x, visibleFrame.minX + 6), visibleFrame.maxX - size.width - 6)
        origin.y = min(max(origin.y, visibleFrame.minY + 6), visibleFrame.maxY - size.height - 6)

        window.setFrameOrigin(origin)
        window.orderFrontRegardless()

        // 放置されたら自動で消す
        dismissTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 4_000_000_000)
            guard !Task.isCancelled else { return }
            self?.dismiss()
        }
    }

    func dismiss() {
        dismissTask?.cancel()
        dismissTask = nil
        action = nil
        window?.orderOut(nil)
    }

    private func performAction() {
        let action = action
        dismiss()
        action?()
    }
}

private struct SelectionBubbleView: View {
    /// 添え書き（選択した文字数など）。nilならアイコンだけの丸
    let label: String?
    let showsIcon: Bool
    /// nilなら表示専用（クリックで何も起きない）
    let action: (() -> Void)?

    var body: some View {
        Group {
            if let action {
                Button(action: action) { content }
                    .buttonStyle(.plain)
                    .accessibilityLabel("iroha")
            } else {
                content
            }
        }
        .padding(2)
    }

    private var content: some View {
        HStack(spacing: 6) {
            if showsIcon {
                Image(systemName: "sparkles")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Color.accentColor)
            }
            if let label {
                Text(label)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(.primary)
                    .lineLimit(1)
                    .fixedSize()
            }
        }
        .frame(minWidth: 34, minHeight: 34)
        .padding(.horizontal, label == nil ? 0 : 10)
        .background(
            Capsule().fill(Color(nsColor: .windowBackgroundColor).opacity(0.97))
        )
        .overlay(
            Capsule().stroke(Color(nsColor: .separatorColor), lineWidth: 1)
        )
    }
}

/// プリセットの選択メニュー（バブルのクリック、または選択ジェスチャから直接開く）
@MainActor
final class SelectionMenuController: NSObject {
    private var action: ((SelectionMenuChoice) -> Void)?

    func present(
        at location: NSPoint,
        presets: [SelectionPreset],
        action: @escaping (SelectionMenuChoice) -> Void
    ) {
        self.action = action

        let menu = NSMenu(title: "iroha")
        menu.autoenablesItems = false

        let header = NSMenuItem(title: "iroha", action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)
        menu.addItem(.separator())

        let onDemandItem = NSMenuItem(
            title: "AIに指示...",
            action: #selector(selectOnDemand),
            keyEquivalent: ""
        )
        onDemandItem.target = self
        onDemandItem.image = NSImage(systemSymbolName: "text.cursor", accessibilityDescription: nil)
        menu.addItem(onDemandItem)

        let enabledPresets = presets.filter(\.enabled)
        if !enabledPresets.isEmpty {
            menu.addItem(.separator())
        }
        for preset in enabledPresets {
            let item = NSMenuItem(
                title: menuTitle(for: preset),
                action: #selector(selectPreset(_:)),
                keyEquivalent: ""
            )
            item.target = self
            item.representedObject = NSNumber(value: preset.index)
            item.image = NSImage(systemSymbolName: "wand.and.stars", accessibilityDescription: nil)
            menu.addItem(item)
        }

        menu.popUp(positioning: nil, at: location, in: nil)
        self.action = nil
    }

    @objc private func selectOnDemand() {
        action?(.onDemand)
    }

    @objc private func selectPreset(_ sender: NSMenuItem) {
        guard let number = sender.representedObject as? NSNumber else { return }
        action?(.preset(index: number.intValue))
    }

    private func menuTitle(for preset: SelectionPreset) -> String {
        let limit = 42
        let name = preset.displayName.replacingOccurrences(of: "\n", with: " ")
        return name.count <= limit ? name : String(name.prefix(limit)) + "…"
    }
}
