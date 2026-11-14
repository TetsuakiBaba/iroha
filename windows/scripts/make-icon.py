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

# タスクバーの入力モードの表示（かなは「い」、英数は「A」）。macOS版のメニューバーの表示（main.tiff・en.tiff の
# 16px と 32px。黒い形と透明の背景）から作る。TIP が読み込んで、テーマの文字色で塗り直して出す
for name, tiff in (("mode-kana.ico", "main.tiff"), ("mode-alphanumeric.ico", "en.tiff")):
    image = Image.open(root / "macos" / "Resources" / tiff)
    frames = {}
    for index in range(getattr(image, "n_frames", 1)):
        image.seek(index)
        frames[image.size[0]] = image.convert("RGBA").copy()
    large = frames[max(frames)]
    path = output.parent / name
    # 16px は描き分けた絵をそのまま使い、ほかの大きさは 32px から縮める
    images = [frames[16] if s == 16 and 16 in frames else large.resize((s, s), Image.LANCZOS) for s in (16, 20, 24)]
    large.save(path, sizes=[(s, s) for s in (16, 20, 24, 32)], append_images=images)
    print(f"wrote {path}")
