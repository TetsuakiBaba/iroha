#pragma once
// 選択テキストのAI編集（ほかのアプリで選んだ文章を、グローバルショートカットで AI に渡して置き換える。既定 OFF）。
// 移植元: macos/Sources/iroha/Selection/（SelectionActionCoordinator・SelectionService・SelectionResultPanel）。
//
// 変換サーバの中に専用のスレッドと窓を持つ。ショートカットで使うときの選択の取り方は macOS版と違い、クリップボード経由:
// クリップボードの中身を控えて Ctrl+C で選択を取り、結果は元のアプリに戻して Ctrl+V で貼り、控えた中身を戻す
// （書き込む中身にはクリップボードの履歴に残さない印を付ける）。
//
// マウスで選んだとき（ドラッグ・ダブルクリック）は、macOS版と同じくアイコン（とメニュー）を出す（macOS版の
// MouseSelectionMonitor・SelectionBubbleController）。マウスは低水準のフックで見て、選んだ文章は UI Automation で読む
// （クリップボードは使わない）。UI Automation で文章を読めないアプリでは、入力欄・文書の上で選んだときだけアイコンを出し、
// メニューで選んだ時点で Ctrl+C で取る

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ai_client.h"
#include "iroha/ai_conversion.h"

namespace iroha {

struct SelectionAssistantSettings {
    bool enabled = false;
    struct Preset {
        std::u32string name;
        std::u32string prompt; // 空なら既定のプロンプト
        std::string hotkey;    // "Ctrl+Alt+1" の形（空なら割り当てなし）
        bool enabled = true;
    };
    std::vector<Preset> presets;
    std::string onDemandHotkey; // その場でプロンプトを入れるキー（空なら割り当てなし）
    std::vector<std::wstring> excludedApps; // 使わないアプリの実行ファイルの名前（小文字）
    // マウスで選んだときの出し方（macOS版の SelectionTriggerMode。Windows では ⌥ の代わりに Alt）
    enum class Trigger { Off, Bubble, ImmediateMenu, AltMenu };
    Trigger trigger = Trigger::Bubble;
    // 選んだ文字数をアイコンに添える（AI編集が OFF でも、文字数だけを出す）
    bool characterCount = false;
};

class SelectionAssistant {
public:
    // AI に依頼する（途中経過を onPartial に渡す。cancel で止められる）
    using Runner = std::function<AIResult(const AIRequest& request,
                                          const std::function<void(const std::u32string&)>& onPartial,
                                          CancelToken* cancel)>;
    // AI の接続先が設定されているか・どれかのアプリで入力中か・記録を残す
    using Check = std::function<bool()>;
    using Logger = std::function<void(const std::string&)>;

    SelectionAssistant(Runner runner, Check available, Check composing, Logger log);
    ~SelectionAssistant();
    SelectionAssistant(const SelectionAssistant&) = delete;
    SelectionAssistant& operator=(const SelectionAssistant&) = delete;

    // 設定を変える（ショートカットを登録し直す。どのスレッドから呼んでもよい）
    void Configure(SelectionAssistantSettings settings);
    // マウスポインタの近くに知らせを少しのあいだ出す（どのスレッドから呼んでもよい）
    void ShowNotice(const std::u32string& text, int milliseconds);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace iroha
