#pragma once
// 「Alt+Enter」のようなショートカットの文字列（AI変換のプリセットのキー）。
// 書き方は macOS版の GlobalShortcut と同じで、Option・Opt は Alt、Control は Ctrl として読む（Cmd は Windows に無いので受けない）。
// 変換サーバが解釈して数値にし、TIP に渡す（TIP は設定ファイルを読めないアプリの中でも動くため）

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

    // 「Ctrl+Alt+P」の形に書く（Parse の逆。表示用）
    std::string Text() const {
        std::string text;
        if (control) text += "Ctrl+";
        if (alt) text += "Alt+";
        if (shift) text += "Shift+";
        if (vk == 0x0D) return text + "Enter";
        if (vk == 0x20) return text + "Space";
        if (vk == 0x09) return text + "Tab";
        if (vk >= 0x70 && vk <= 0x7B) return text + "F" + std::to_string(vk - 0x70 + 1);
        return text + static_cast<char>(vk);
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
