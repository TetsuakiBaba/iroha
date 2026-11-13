#pragma once
// 「Alt+Enter」のようなショートカットの文字列（AI変換のプリセットのキー・AIで直す・選択テキストのAI編集）。
// 書き方は macOS版の GlobalShortcut と同じで、Option・Opt は Alt、Control は Ctrl として読む（Cmd は受けない）。
// Windows版から写したもの。数値の形（vk は Windows の仮想キーコードの値）も同じにしてあり、Linux では
// IBus / X のキーの値（keysym）と照合する（Matches・Keysym）

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace iroha {

struct KeyShortcut {
    uint32_t vk = 0; // 仮想キーコード
    bool control = false;
    bool alt = false;
    bool shift = false;

    // 修飾キーが 1 つ以上ないものは受けない（Enter だけを割り当てると普通の確定ができなくなる）
    static std::optional<KeyShortcut> Parse(const std::string& text) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : text + "+") {
            if (c == '+') {
                size_t start = 0;
                size_t end = current.size();
                while (start < end && current[start] == ' ') ++start;
                while (end > start && current[end - 1] == ' ') --end;
                parts.push_back(Lower(current.substr(start, end - start)));
                current.clear();
            } else {
                current += c;
            }
        }
        if (parts.size() < 2) return std::nullopt;
        KeyShortcut shortcut;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            const std::string& part = parts[i];
            if (part == "ctrl" || part == "control") {
                shortcut.control = true;
            } else if (part == "alt" || part == "option" || part == "opt") {
                shortcut.alt = true;
            } else if (part == "shift") {
                shortcut.shift = true;
            } else {
                return std::nullopt;
            }
        }
        const std::string& key = parts.back();
        if (key == "enter" || key == "return") {
            shortcut.vk = 0x0D;
        } else if (key == "space") {
            shortcut.vk = 0x20;
        } else if (key == "tab") {
            shortcut.vk = 0x09;
        } else if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') {
            shortcut.vk = static_cast<uint32_t>(key[0] - 'a' + 'A');
        } else if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
            shortcut.vk = static_cast<uint32_t>(key[0]);
        } else if (key.size() >= 2 && key.size() <= 3 && key[0] == 'f') {
            const int number = std::atoi(key.c_str() + 1);
            if (number < 1 || number > 12) return std::nullopt;
            shortcut.vk = 0x70 + static_cast<uint32_t>(number - 1);
        } else {
            return std::nullopt;
        }
        if (!shortcut.control && !shortcut.alt && !shortcut.shift) return std::nullopt;
        return shortcut;
    }

    // IBus のキー（keyval と修飾キーの状態）がこのショートカットか。修飾キーは Ctrl・Alt・Shift の 3 つを比べる
    bool Matches(uint32_t keyval, uint32_t modifiers) const {
        const bool hasShift = (modifiers & (1u << 0)) != 0;
        const bool hasControl = (modifiers & (1u << 2)) != 0;
        const bool hasAlt = (modifiers & (1u << 3)) != 0;
        return vk != 0 && VirtualKeyOf(keyval) == vk && hasShift == shift && hasControl == control && hasAlt == alt;
    }

    // X のキーの値（keysym）。キーをつかむ（XGrabKey）のに使う
    uint32_t Keysym() const {
        if (vk == 0x0D) return 0xff0d; // Return
        if (vk == 0x20) return 0x20;
        if (vk == 0x09) return 0xff09; // Tab
        if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 'a';
        if (vk >= '0' && vk <= '9') return vk;
        if (vk >= 0x70 && vk <= 0x7B) return 0xffbe + (vk - 0x70); // F1〜F12
        return 0;
    }

    // keysym を vk の値にする（当たらなければ 0）
    static uint32_t VirtualKeyOf(uint32_t keyval) {
        if (keyval == 0xff0d || keyval == 0xff8d) return 0x0D;      // Return・KP_Enter
        if (keyval == 0x20 || keyval == 0xff80) return 0x20;        // space・KP_Space
        if (keyval == 0xff09 || keyval == 0xfe20) return 0x09;      // Tab・ISO_Left_Tab
        if (keyval >= 'a' && keyval <= 'z') return keyval - 'a' + 'A';
        if (keyval >= 'A' && keyval <= 'Z') return keyval;
        if (keyval >= '0' && keyval <= '9') return keyval;
        if (keyval >= 0xffbe && keyval <= 0xffc9) return 0x70 + (keyval - 0xffbe); // F1〜F12
        return 0;
    }

    uint32_t Encode() const {
        return vk | (control ? 1u << 16 : 0) | (alt ? 1u << 17 : 0) | (shift ? 1u << 18 : 0);
    }
    static KeyShortcut Decode(uint32_t value) {
        return KeyShortcut{value & 0xFFFF, (value & (1u << 16)) != 0, (value & (1u << 17)) != 0,
                           (value & (1u << 18)) != 0};
    }

private:
    static std::string Lower(std::string text) {
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return text;
    }
};

} // namespace iroha
