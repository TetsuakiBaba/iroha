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
ibus-iroha/     IBus のエンジン（ibus-engine-iroha）
                  key_decision  キーの読み替え（IBus に依存しない。Windows版 TIP の DecideKey と同じ規則）
                  engine        IBusEngine のサブクラス。キーをセッションに送り、返ってきた表示を IBus に反映する
                  main          エンジン一式を読み込んでから IBus につなぐ（--ibus なしで起動すると開発用に自分で登録する）
                  tests/        キーの読み替えの単体テストと、IBus を通して試す ibus_e2e.py
shared/         専門辞書・打ち間違い訂正モデルの取得（curl、無ければ wget のコマンドで落とす）
scripts/        build-llama.sh（vendor/dist-linux へ）・fetch-model.sh・install.sh
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
./linux/build/ibus-iroha/ibus-iroha-tests
```

### IBus のエンジン

```sh
# インストール（sudo。/usr/local/lib/ibus-iroha と /usr/share/ibus/component/iroha.xml。ibus-daemon の起動し直しまで行う）
./linux/scripts/install.sh
# 「設定 > キーボード > 入力ソース」で「日本語 (iroha)」を足す。またはコマンドで切り替える
ibus engine iroha
# 外す
./linux/scripts/install.sh --uninstall

# インストールせずに試す: 開発用のエンジンを引数なしで起動すると、IBus に「iroha-dev」として自分で登録する
# （インストールしたものと並べられる）。確定は学習・入力履歴に残るので、データフォルダを分けて起動する
mkdir -p /tmp/iroha-e2e
IROHA_DATA_DIR=/tmp/iroha-e2e IROHA_MODEL=$HOME/.local/share/iroha/models/iroha-kkc-v1-Q5_K_M.gguf \
    ./linux/build/ibus-iroha/ibus-engine-iroha &
# IBus を通してキーを送り、アプリが受け取るもの（確定・未確定文字列・候補）を見る（テスト用の入力コンテキストを使い、
# 終わったら全体のエンジンを元に戻す。既定は iroha-dev。インストールしたものは --engine iroha）
/usr/bin/python3 linux/ibus-iroha/tests/ibus_e2e.py "kyouhaiitenkidesune<space><right><space><down><enter>"
```

- **ibus_e2e.py はデスクトップで入力しているときに走らせない**。IBus 1.5 はエンジンをすべての入力コンテキストで共有するので、
  テストが入力中のアプリのエンジンを切り替え、終わったときに元のエンジンへ戻す（入力中に iroha へ切り替えていると外れる）。
  逆に、デスクトップでエンジンを切り替えるとテストの途中で合成が消える（2026-11-14 に起きた）
- **`ibus restart` を使わない**。GNOME（Ubuntu 22.04・IBus 1.5.26）が起動した ibus-daemon で使うと、止まったまま戻らず、
  デスクトップのアプリで入力できなくなった（2026-11-14）。install.sh は、動いている ibus-daemon と同じ引数・環境変数で
  `--replace --daemonize` を付けて起動し直す
- 実行ファイルと同じフォルダに `Dictionary`・`Symbols` を置く（install.sh が置く）。変換モデルはデータフォルダ
- 記録は `<データフォルダ>/logs/ibus-iroha-<ホスト名>.log`（1MB を超えたら起動のときに 1 世代残して作り直す）
- 入力の状態はセッション（`InputSession`）にあり、エンジンは Windows版の TIP と同じく、キーを送って返ってきた表示を
  反映するだけ。入力の動きを変えるときは `iroha-core` の `InputSession` を直す
- 表示の対応: 未確定文字列は下線、選んでいる文節は背景色。候補ウィンドウは IBus の候補表（今のページの 9 件だけを渡す。
  クリックの番号がページの中の番号になるように）。候補の位置（10 件以上のとき）・予測変換・打ち間違いの訂正・
  句読点などの知らせは補助テキスト（候補の上の 1 行）に出す
- フォーカスが外れたときは、IBus が表示中の未確定文字列をそのまま確定する（`IBUS_ENGINE_PREEDIT_COMMIT`）。セッションは
  Reset で捨てる。ほかのエンジンに切り替えたとき（disable）は、今の表示のまま確定する
- 入力モード（あ / A）とプライベートモードはプロパティ（GNOME の上のバーの入力ソースのメニュー）。英数モードは
  半角/全角キー・英数キー・Alt+` で切り替える。英数モードは入力コンテキストごとで、有効にするたびにかなに戻る
- 左文脈は周辺テキスト（surrounding text）から読む。アプリが渡さなければ直前に確定した文字列を使う（セッションの既定）。
  確定の取り消し（Ctrl+Backspace）は周辺テキストでカーソルの手前を確かめてから `delete_surrounding_text` で消す

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

IBus のエンジン（2026-11-14、`ibus_e2e.py`、IBus 1.5.26）: ライブ変換・文節変換・候補の選択・確定、F7・Esc・Backspace、
英数モードの切り替え、Ctrl+S の前の確定、確定の取り消し（Ctrl+Backspace）、周辺テキストからの左文脈が期待どおりに動いた。
エンジンの起動（モデル・辞書・記号の読み込み）は 377ms。インストールした配置（`DESTDIR` に置いたもの）でも、
実行ファイルの隣の `Dictionary`・`Symbols` を読むことを確かめた。**実際のアプリ（GNOME の候補ウィンドウの表示・
GTK / Qt / ブラウザ / 端末での動き）はまだ確かめていない**

モデルの結果の 1 件の差は、計算機の違いによる対数確率のずれとみられる（Windows版の README にも、macOS版との採点の差が
2% ほどあると記録がある）。照合は未実施（`model-parity` は macOS版の `ajimee-dump` の出力が要る）。
計測時は学習の処理が動いていて、負荷平均は 5〜6 だった（速さは参考値）。

## これからの段階

1. [x] 段階 1: コアと iroha-cli を Linux でビルドし、テストと評価を通す
2. [x] 段階 2: IBus のエンジン（`ibus-iroha/`）と install.sh。IBus を通した動作の確認まで
   - [ ] 実際のアプリで確かめる（GNOME の候補ウィンドウ、GTK・Qt・Firefox / Chrome・端末、Wayland）
   - [ ] 設定画面をプロパティから開く（段階 3 のあと）
3. [ ] 段階 3: 設定画面（GTK4）。Windows版と同じタブ。取得は libcurl に替える（`libcurl4-openssl-dev`）
4. [ ] 段階 4: 描けない絵文字を除く（fontconfig で Noto Color Emoji の字形を見る）・起動の記録
5. [ ] 段階 5: 配布（install.sh → tar.gz / .deb、GitHub Release への添付は CLAUDE.md のリリース方針に Linux を足して行う）
6. [ ] 段階 6（任意）: fcitx5

未対応（macOS版・Windows版にあるもの）: 描けない絵文字の除外・設定画面・AI の機能・「こえ」モード・
候補ウィンドウを表に広げる（Tab）・区切り記号で囲んだ英字入力・起動の記録（`LaunchLog`）・自動アップデート。
