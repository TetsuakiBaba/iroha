#!/usr/bin/env python3
"""上のバーの iroha のアイコン（「い」「A」）を macOS版の入力モードのアイコンから作る。

macOS版の macos/Resources/main.tiff（かな、「い」）と en.tiff（英数、「A」）は、黒の字形を透明度で描いた
テンプレート画像（16px と 32px の 2 枚）。その透明度だけを linux/ibus-iroha/mode_icons.h に書き出す
（色はエンジンが付ける。上のバーは暗い色なので白）。22px・24px は 32px から縮小して作る。
アイコンの絵を変えたら macOS版の .tiff を直し、これを流し直してコミットする。

    python3 linux/scripts/make-mode-icons.py    （要 Pillow）
"""
import os
import sys

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SOURCES = [("kKanaIcon", "macos/Resources/main.tiff"), ("kDirectIcon", "macos/Resources/en.tiff")]
OUTPUT = os.path.join(ROOT, "linux/ibus-iroha/mode_icons.h")
SIZES = [16, 22, 24, 32]


def frames(path):
    image = Image.open(path)
    result = {}
    index = 0
    while True:
        try:
            image.seek(index)
        except EOFError:
            break
        frame = image.convert("RGBA")
        result[frame.size[0]] = frame.getchannel("A")
        index += 1
    return result


def alpha_for(found, size):
    if size in found:
        return found[size]
    largest = max(found)
    return found[largest].resize((size, size), Image.LANCZOS)


def main():
    lines = [
        "#pragma once",
        "// 上のバーの iroha のアイコン（入力モードごと・大きさごとの字形の透明度。0〜255、行の順）。",
        "// linux/scripts/make-mode-icons.py が macOS版の macos/Resources/main.tiff（「い」）・en.tiff（「A」）から作る。",
        "// 手で直さない",
        "#include <cstdint>",
        "",
        "namespace iroha::ibus::mode_icons {",
        "",
        "struct Icon {",
        "    int size;",
        "    const uint8_t* alpha; // size × size",
        "};",
        "",
    ]
    for name, source in SOURCES:
        found = frames(os.path.join(ROOT, source))
        if not found:
            sys.exit(f"{source} に画像がありません")
        entries = []
        for size in SIZES:
            data = list(alpha_for(found, size).getdata())
            array = f"{name}{size}"
            lines.append(f"inline constexpr uint8_t {array}[] = {{")
            for y in range(size):
                row = data[y * size:(y + 1) * size]
                lines.append("    " + ",".join(str(v) for v in row) + ",")
            lines.append("};")
            entries.append(f"Icon{{{size}, {array}}}")
        lines.append(f"inline constexpr Icon {name}[] = {{{', '.join(entries)}}};")
        lines.append("")
    lines.append("} // namespace iroha::ibus::mode_icons")
    with open(OUTPUT, "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    print(f"書き出しました: {os.path.relpath(OUTPUT, ROOT)}")


if __name__ == "__main__":
    main()
