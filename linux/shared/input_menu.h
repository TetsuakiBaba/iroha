#pragma once
// 入力メニュー（GNOME のトップバー・IBus のパネル・fcitx5 のトレイから開くメニュー）の項目と並び。
// macOS版の入力メニュー（IrohaInputController.menu）と同じ並びにする:
//   かな / 英数（macOS では入力ソースの一覧に出るもの）
//   設定… / ユーザ辞書… / 変換ルール…
//   プライベートモード / ライブ変換 / 予測変換 / 句読点スタイル
//   モデルフォルダを開く
//   変換モデルの状況（見つからないときだけ） / iroha の版
// macOS版にあって Linux版に無いもの: 「原稿に合わせる」（機能が未移植）・「アップデートを確認…」（アップデータが無い）。
// IBus と fcitx5 のエンジンがこの並びをそれぞれの形にする（画面に頼らないので単体テストする）
#include <string>
#include <vector>

namespace iroha::menu {

enum class Command {
    None,           // 押せない項目（区切り線・状況・版）
    KanaMode,       // かなにする
    DirectMode,     // 英数にする
    Settings,       // 設定画面を開く
    UserDictionary, // 設定画面のユーザ辞書のタブを開く
    RewriteRules,   // 設定画面の変換ルールのタブを開く
    PrivateMode,    // プライベートモードを切り替える
    LiveConversion, // ライブ変換を切り替える
    Predictive,     // 予測変換を切り替える
    Punctuation,    // 句読点を切り替える（Ctrl+. と同じ）
    ModelFolder,    // モデルフォルダを開く
};

struct Item {
    enum class Type {
        Radio,     // かな・英数（選んでいる方に印）
        Normal,    // 押すと動く
        Toggle,    // ON・OFF（印で状態を出す）
        Separator, // 区切り線
        Status,    // 押せない文字の行
    };
    Type type = Type::Normal;
    // 項目の名前（IBus のプロパティのキー・fcitx5 のアクションの名前に使う。メニューの中で重ならない）。
    // かなの項目は "InputMode"（GNOME と IBus のパネルは、このキーの記号「あ」「A」をトップバーに出す）
    std::string key;
    std::string label;
    bool checked = false;
    Command command = Command::None;
};

// メニューを組むのに使う、今の状態
struct State {
    bool directMode = false;
    bool privateMode = false;
    bool liveConversion = true;
    bool predictive = false;
    std::string punctuation = "、。";
    // キーの文字列（空なら割り当てなし。項目の名前の後ろに出す）
    std::string inputModeShortcut;
    std::string privateModeShortcut;
    // 変換モデルのファイルが無い
    bool modelMissing = false;
    std::string version;
};

std::vector<Item> Build(const State& state);
// 項目の名前から何をするか（知らない名前なら None）
Command CommandFor(const std::string& key);
// 入力モードの記号（トップバーに出す）
inline const char* ModeSymbol(bool directMode) { return directMode ? "A" : "あ"; }

} // namespace iroha::menu
