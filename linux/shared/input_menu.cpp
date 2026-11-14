#include "input_menu.h"

namespace iroha::menu {

namespace {

// 「名前（キー）」。キーが無ければ名前だけ（macOS版はメニューの右端にキーを出す）
std::string WithShortcut(const std::string& label, const std::string& shortcut) {
    return shortcut.empty() ? label : label + "（" + shortcut + "）";
}

Item Make(Item::Type type, const std::string& key, const std::string& label, Command command, bool checked = false) {
    Item item;
    item.type = type;
    item.key = key;
    item.label = label;
    item.command = command;
    item.checked = checked;
    return item;
}

Item Separator(int number) { return Make(Item::Type::Separator, "Separator" + std::to_string(number), "", Command::None); }

} // namespace

std::vector<Item> Build(const State& state) {
    std::vector<Item> items;
    // 切り替えのキーは、今と反対のモードの項目に付ける（押すとそちらに移るので）
    items.push_back(Make(Item::Type::Radio, "InputMode",
                         WithShortcut("かな", state.directMode ? state.inputModeShortcut : ""), Command::KanaMode,
                         !state.directMode));
    items.push_back(Make(Item::Type::Radio, "InputMode.Direct",
                         WithShortcut("英数", state.directMode ? "" : state.inputModeShortcut), Command::DirectMode,
                         state.directMode));
    items.push_back(Separator(1));
    items.push_back(Make(Item::Type::Normal, "Setup", "設定…", Command::Settings));
    items.push_back(Make(Item::Type::Normal, "UserDictionary", "ユーザ辞書…", Command::UserDictionary));
    items.push_back(Make(Item::Type::Normal, "RewriteRules", "変換ルール…", Command::RewriteRules));
    items.push_back(Separator(2));
    items.push_back(Make(Item::Type::Toggle, "PrivateMode", WithShortcut("プライベートモード", state.privateModeShortcut),
                         Command::PrivateMode, state.privateMode));
    items.push_back(Make(Item::Type::Toggle, "LiveConversion", "ライブ変換", Command::LiveConversion,
                         state.liveConversion));
    items.push_back(Make(Item::Type::Toggle, "Predictive", "予測変換", Command::Predictive, state.predictive));
    items.push_back(Make(Item::Type::Normal, "Punctuation", WithShortcut("句読点スタイル: " + state.punctuation, "Ctrl+."),
                         Command::Punctuation));
    items.push_back(Separator(3));
    items.push_back(Make(Item::Type::Normal, "ModelFolder", "モデルフォルダを開く", Command::ModelFolder));
    items.push_back(Separator(4));
    if (state.modelMissing) {
        items.push_back(Make(Item::Type::Status, "ModelStatus", "変換モデルが見つかりません（設定 > モデル）", Command::None));
    }
    // モデル名は出さない（macOS版と同じ。アダプタを当てると名前が長くなりメニューが横に広がる）
    items.push_back(Make(Item::Type::Status, "Version", "iroha " + state.version, Command::None));
    return items;
}

Command CommandFor(const std::string& key) {
    static const struct {
        const char* key;
        Command command;
    } kCommands[] = {
        {"InputMode", Command::KanaMode},
        {"InputMode.Direct", Command::DirectMode},
        {"Setup", Command::Settings},
        {"UserDictionary", Command::UserDictionary},
        {"RewriteRules", Command::RewriteRules},
        {"PrivateMode", Command::PrivateMode},
        {"LiveConversion", Command::LiveConversion},
        {"Predictive", Command::Predictive},
        {"Punctuation", Command::Punctuation},
        {"ModelFolder", Command::ModelFolder},
    };
    for (const auto& entry : kCommands) {
        if (key == entry.key) return entry.command;
    }
    return Command::None;
}

} // namespace iroha::menu
