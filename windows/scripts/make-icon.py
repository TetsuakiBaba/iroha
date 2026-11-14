# Windows版のアイコン windows/resources/iroha.ico を macOS版のアプリのアイコン（macos/Resources/AppIcon.icns）から作る。
#   python windows/scripts/make-icon.py
# macOS のアイコンは周りに余白を持つ（Big Sur 以降の格子）ので、Windows では余白を詰めて大きさいっぱいに描く。
# TIP（言語の切り替え・タスクバーの入力インジケーター）・設定画面・変換サーバ・インストーラーが同じものを使う
# 要るもの: Pillow

from pathlib import Path

from PIL import Image

root = Path(__file__).resolve().parents[2]
source = root / "macos" / "Resources" / "AppIcon.icns"
output = root / "windows" / "resources" / "iroha.ico"

icon = Image.open(source)
icon.size = (512, 512)  # 512@2x（1024px）を選ぶ
icon.load()
icon = icon.convert("RGBA")
left, top, right, bottom = icon.getchannel("A").getbbox()
side = max(right - left, bottom - top)
square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
square.alpha_composite(icon.crop((left, top, right, bottom)), ((side - (right - left)) // 2, (side - (bottom - top)) // 2))
output.parent.mkdir(parents=True, exist_ok=True)
square.save(output, sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 256)])
print(f"wrote {output}")
