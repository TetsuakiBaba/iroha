#include "selection_rules.h"

#include <algorithm>
#include <cstdlib>

#include "iroha/unicode.h"

namespace iroha::selection {

Settings SettingsFromConfig(const Config& config) {
    Settings settings;
    settings.enabled = config.selectionEnabled;
    const auto& defaults = SelectionSettingsDefaults::Presets();
    for (size_t i = 0; i < config.selectionPresets.size() && i < defaults.size(); ++i) {
        const auto& stored = config.selectionPresets[i];
        const auto& fallback = defaults[i];
        settings.presets.push_back({stored.name.value_or(fallback.name), stored.prompt.value_or(fallback.prompt),
                                    stored.hotkey.value_or(fallback.hotkey), stored.enabled.value_or(fallback.enabled)});
    }
    settings.onDemandHotkey = config.selectionOnDemandHotkey.value_or(SelectionSettingsDefaults::kOnDemandHotkey);
    settings.excludedApps = ParseExcludedApps(config.selectionExcludedApps);
    settings.trigger = ParseTrigger(config.selectionTriggerMode);
    settings.characterCount = config.selectionCharacterCount;
    return settings;
}

Settings::Trigger ParseTrigger(const std::string& value) {
    if (value == "off") return Settings::Trigger::Off;
    if (value == "immediateMenu") return Settings::Trigger::ImmediateMenu;
    if (value == "optionMenu") return Settings::Trigger::AltMenu;
    return Settings::Trigger::Bubble;
}

void GestureTracker::Press(int x, int y) {
    down_ = true;
    downX_ = x;
    downY_ = y;
}

bool GestureTracker::Release(int x, int y, unsigned long time) {
    const long dx = x - downX_;
    const long dy = y - downY_;
    const bool dragged = down_ && dx * dx + dy * dy >= static_cast<long>(dragDistance_) * dragDistance_;
    const bool doubleClick = hasLastUp_ && time >= lastUpTime_ && time - lastUpTime_ <= doubleClickTime_ &&
                             std::abs(x - lastUpX_) <= doubleClickDistance_ && std::abs(y - lastUpY_) <= doubleClickDistance_;
    const bool gesture = down_ && (dragged || doubleClick);
    // 3 回目のクリックを 2 回目と数えない（ダブルクリックのあとは数え直す）。ドラッグのあとも数え直す
    hasLastUp_ = !(doubleClick || dragged);
    lastUpTime_ = time;
    lastUpX_ = x;
    lastUpY_ = y;
    down_ = false;
    return gesture;
}

bool ShouldUseMouseSelection(const std::optional<std::u32string>& text, bool canWatchOwner, bool ownerChanged,
                             const std::u32string& previous) {
    if (!text || TrimWhitespace(*text).empty()) return false;
    return canWatchOwner ? ownerChanged : *text != previous;
}

std::string Lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::vector<std::string> ParseExcludedApps(const std::string& text) {
    std::vector<std::string> names;
    std::string current;
    for (char c : text + ",") {
        if (c == ',' || c == '\n' || c == '\r') {
            const std::string name = Lower(Utf32ToUtf8(TrimWhitespace(Utf8ToUtf32(current))));
            if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            current.clear();
        } else {
            current += c;
        }
    }
    return names;
}

bool IsExcluded(const std::vector<std::string>& excludedApps, const AppIdentity& app) {
    const std::string names[] = {Lower(app.command), Lower(app.classInstance), Lower(app.className)};
    for (const std::string& name : names) {
        if (name.empty()) continue;
        // iroha 自身（設定画面・この常駐プログラム）では使わない
        if (name.rfind("iroha", 0) == 0) return true;
        if (std::find(excludedApps.begin(), excludedApps.end(), name) != excludedApps.end()) return true;
    }
    return false;
}

unsigned ModifierMask(const KeyShortcut& shortcut) {
    return (shortcut.shift ? kShiftMask : 0u) | (shortcut.control ? kControlMask : 0u) | (shortcut.alt ? kAltMask : 0u);
}

std::vector<HotkeyBinding> PlanHotkeys(const Settings& settings) {
    std::vector<HotkeyBinding> bindings;
    if (!settings.enabled) return bindings;
    const auto add = [&bindings](int id, const std::string& text) {
        const auto shortcut = KeyShortcut::Parse(text);
        if (!shortcut) return;
        const uint32_t keysym = shortcut->Keysym();
        if (keysym == 0) return;
        const unsigned modifiers = ModifierMask(*shortcut);
        for (const HotkeyBinding& existing : bindings) {
            if (existing.keysym == keysym && existing.modifiers == modifiers) return;
        }
        bindings.push_back({id, text, keysym, modifiers});
    };
    for (size_t i = 0; i < settings.presets.size(); ++i) {
        const auto& preset = settings.presets[i];
        if (preset.enabled && !preset.hotkey.empty()) add(static_cast<int>(i), preset.hotkey);
    }
    if (!settings.onDemandHotkey.empty()) add(kOnDemandId, settings.onDemandHotkey);
    return bindings;
}

std::vector<unsigned> LockVariants(unsigned modifiers, unsigned numLockMask) {
    std::vector<unsigned> variants = {modifiers, modifiers | kLockMask};
    if (numLockMask != 0 && numLockMask != kLockMask) {
        variants.push_back(modifiers | numLockMask);
        variants.push_back(modifiers | numLockMask | kLockMask);
    }
    return variants;
}

int FindHotkey(const std::vector<GrabbedKey>& keys, unsigned keycode, unsigned state, unsigned numLockMask) {
    const unsigned relevant = kShiftMask | kControlMask | kAltMask;
    const unsigned pressed = state & relevant & ~numLockMask;
    for (const GrabbedKey& key : keys) {
        if (key.keycode == keycode && key.modifiers == pressed) return key.id;
    }
    return -1;
}

std::u32string PresetHeading(const Settings::Preset& preset, size_t index) {
    const std::u32string name = TrimWhitespace(preset.name);
    if (!name.empty()) return name;
    return U"プリセット" + Utf8ToUtf32(std::to_string(index + 1));
}

AIRequest PresetRequest(const Settings::Preset& preset, size_t index, const std::u32string& source) {
    std::u32string prompt = preset.prompt;
    const auto& defaults = SelectionSettingsDefaults::Presets();
    if (TrimWhitespace(prompt).empty() && index < defaults.size()) prompt = defaults[index].prompt;
    return AIRequest::Build(prompt, source);
}

AIRequest PromptRequest(const std::u32string& instruction, const std::u32string& source, bool generation) {
    if (generation) {
        AIRequest request;
        request.instructions =
            std::u32string(SelectionSettingsDefaults::kGenerationInstructions) + AICommitSettings::kOutputRule;
        request.userMessage = instruction;
        return request;
    }
    return AIRequest::Build(instruction, source);
}

std::u32string SourceLine(const std::u32string& source, bool generation) {
    if (generation) return U"（選択なし: 指示から文章を作り、カーソルの位置に入れます）";
    std::u32string shown = source;
    std::replace(shown.begin(), shown.end(), U'\n', U' ');
    std::replace(shown.begin(), shown.end(), U'\r', U' ');
    std::replace(shown.begin(), shown.end(), U'\t', U' ');
    return U"選択: " + shown;
}

std::optional<std::u32string> SelectionFromPrimary(const std::optional<std::string>& utf8) {
    if (!utf8) return std::nullopt;
    std::u32string text = Utf8ToUtf32(*utf8);
    if (TrimWhitespace(text).empty()) return std::nullopt;
    return text;
}

} // namespace iroha::selection
