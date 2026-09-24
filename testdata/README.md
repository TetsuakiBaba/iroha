# testdata

**このディレクトリの役割: 評価セット（Git で追跡する）。** 学習データを作るのは `../dataset/iroha-typo-normalizer/`、
学習は `../training/`（どちらも作ったデータは追跡しない）。全体の構成はルートの README.md の「ディレクトリ構成」節。

| パス | 中身 | Git |
|---|---|---|
| `eval.tsv` | かな漢字変換の独自評価セット（読み → 期待する変換、40 文） | 追跡 |
| `iroha/typo/` | 打ち間違いの訂正の書き下ろしベンチ（200 件。説明は同ディレクトリの README.md） | 追跡 |
| `ajimee/` | AJIMEE-Bench（`evaluation_items.json`、CC BY-SA 3.0）。取得して置く | 追跡しない |
