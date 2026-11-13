# iroha for Linux（作成中）

Linux の IME。方針（2026-11-13 決定）:

- 変換エンジンは Windows版の C++ 実装（`windows/iroha-core`）を **コミット 2de5ee1 で写し、以後は Linux版として別に保つ**。
  `linux/` から `windows/` は参照しない。仕様の正は macOS版（`macos/Sources/IrohaCore/`）
- フロントエンドは **IBus** を先に作る（GNOME の標準）。fcitx5 は後
- IBus のエンジンは ibus-daemon が起動する別のプロセスなので、Windows版のように変換サーバとアプリ側の部品を分けず、
  エンジンのプロセスに変換エンジン・入力の状態（`SessionManager`）を直接置く
- 対象は当面 x86_64・CPU 推論（llama.cpp は AVX2 まで。手元の CPU 向けの最適化は使わない）

## 構成

```
iroha-core/     変換エンジン（Windows版から写したもの）と Linux で足した部品
                  platform     データフォルダ・実行ファイルの場所・ホスト名・環境変数
                  engine_host  設定からエンジン一式を組み立て、SessionManager に渡す
                               （Windows版の iroha-server の ServerState / ServerBackend を移したもの）
                  sha256       ダウンロードしたモデル・辞書の照合
iroha-cli/      検証用 CLI（remote は無い。session はこのプロセスの中でセッションを動かす）
shared/         専門辞書・打ち間違い訂正モデルの取得（curl、無ければ wget のコマンドで落とす）
scripts/        build-llama.sh（vendor/dist-linux へ）・fetch-model.sh
```

データフォルダは `IROHA_DATA_DIR` > `$XDG_DATA_HOME/iroha` > `~/.local/share/iroha`。中のファイル
（`config.json`・`models/`・`learning.json`・`user-dictionary.json`・`dictionaries/` など）の形式は macOS版・Windows版と同じ。

## ビルドと動作確認

```sh
# 要るパッケージ（IBus のエンジンの分も含む）
sudo apt install cmake ninja-build libibus-1.0-dev libglib2.0-dev

./linux/scripts/build-llama.sh          # llama.cpp を vendor/dist-linux へ（vendor/dist・dist-windows には触らない）
cmake -S linux -B linux/build -G Ninja && cmake --build linux/build
./linux/build/iroha-core/iroha-core-tests

./linux/scripts/fetch-model.sh          # 既定のモデル iroha-kkc-v1 をデータフォルダへ（約82MB、SHA-256 を照合）

C=./linux/build/iroha-cli/iroha-cli
$C convert kyouhaiitenkidesune
$C session "kyouhaiitenkidesune<space><right><space><enter>"
$C bench testdata/eval.tsv
IROHA_LATTICE=off    $C ajimee testdata/ajimee/evaluation_items.json
IROHA_LATTICE=always $C ajimee testdata/ajimee/evaluation_items.json
```

辞書ラティスの辞書（`vendor/azooKey_dictionary_storage`）と記号のデータ（`vendor/mozc-data`）は macOS版の
`macos/scripts/fetch-dictionary.sh`・`fetch-symbols.sh` で取る。見つける順は Windows版と同じ
（環境変数 `IROHA_DICTIONARY` / `IROHA_SYMBOLS` → 実行ファイルと同じフォルダの `Dictionary` / `Symbols` →
カレントディレクトリから上へたどった `vendor/…`）。

## 確認したこと（2026-11-13、Ubuntu 22.04・g++ 11.4・Xeon E5-1650 v4（12 スレッド）、iroha-kkc-v1）

| 評価 | Linux版 | Windows版の記録 |
|---|---|---|
| コアの単体テスト | 1007 件すべて通る（Windows版の 996 件 + SHA-256・パスの 11 件） | — |
| 辞書ラティスの候補の並び（macOS版の出力との照合、AJIMEE 200 件） | n=10・n=30 とも 200/200 一致 | 200/200 一致 |
| AJIMEE モデルのみ / ラティス再採点 | 171 (85.5%) / 170 (85.0%)、248ms / 415ms | 171 (85.5%) / 171 (85.5%)、221ms / 386ms |
| eval.tsv 40 件 | 37 (92.5%)、92ms | 38 (95.0%)、85ms |

モデルの結果の 1 件の差は、計算機の違いによる対数確率のずれとみられる（Windows版の README にも、macOS版との採点の差が
2% ほどあると記録がある）。照合は未実施（`model-parity` は macOS版の `ajimee-dump` の出力が要る）。
計測時は学習の処理が動いていて、負荷平均は 5〜6 だった（速さは参考値）。

## これからの段階

1. [x] 段階 1: コアと iroha-cli を Linux でビルドし、テストと評価を通す
2. [ ] 段階 2: IBus のエンジン（`ibus-iroha/`）。キーの読み替え → `SessionManager::HandleKey` → 未確定文字列・確定・
   候補（`IBusLookupTable`）・補助テキスト。ライブ変換の結果は GLib のタイマーで `Poll` する。左文脈は周辺テキスト
   （surrounding text）、確定の取り消しは `delete_surrounding_text`。入力モード・プライベートモード・設定画面はプロパティ。
   **要るパッケージ（sudo）**: `libibus-1.0-dev libglib2.0-dev`
3. [ ] 段階 3: 設定画面（GTK4）。Windows版と同じタブ。取得は libcurl に替える（`libcurl4-openssl-dev`）
4. [ ] 段階 4: 描けない絵文字を除く（fontconfig で Noto Color Emoji の字形を見る）・起動の記録
5. [ ] 段階 5: 配布（install.sh → tar.gz / .deb、GitHub Release への添付は CLAUDE.md のリリース方針に Linux を足して行う）
6. [ ] 段階 6（任意）: fcitx5

未対応（macOS版・Windows版にあるもの）: 描けない絵文字の除外・設定画面・AI の機能・「こえ」モード、ほか IBus のエンジン全体。
