# iroha for Windows（開発予定）

Windows版irohaをここに実装する。WindowsのIMEはTSF（Text Services Framework）ベースになる。

- 変換エンジン（`macos/Sources/IrohaCore/` 相当）はmacOS専用APIを使っておらず、
  Swift for Windowsでの共有、または別言語での再実装のどちらも選択肢
- llama.cppのソースはルートの `vendor/llama.cpp` を共有する
  （macOS用静的ライブラリ `vendor/dist` はMetal依存のため流用不可。Windows用は別途ビルドする）
- 学習済みモデル（GGUF）・評価データ（`testdata/`）・学習パイプライン（`training/`）は
  プラットフォーム非依存でそのまま使える
