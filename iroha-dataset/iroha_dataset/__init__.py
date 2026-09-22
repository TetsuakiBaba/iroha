"""iroha 用の学習データセット生成パイプライン。

公開データ（Tatoeba / KAKEN …）をダウンロードし、正規化 → 読み生成 → 品質フィルタを
通した canonical record を作り、そこから

  * かな漢字変換用データ（context + input(かな) -> target(漢字かな交じり)）
  * typo normalizer 用データ（崩れたかな -> 正しいかな）

を生成する。データそのものは配布しない（LICENSES.md 参照）。
"""

__version__ = "0.1.0"
