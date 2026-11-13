# iroha for Linux

Linux の IME。方針（2026-11-13 決定）:

- 変換エンジンは Windows版の C++ 実装（`windows/iroha-core`）を **コミット 2de5ee1 で写し、以後は Linux版として別に保つ**。
  `linux/` から `windows/` は参照しない。仕様の正は macOS版（`macos/Sources/IrohaCore/`）。
  2026-11-14 に、Windows版が 2de5ee1 のあとに足したコアの変更（AI変換・候補の表・区切り記号の英字入力・AIで直す・
  選択テキストのAI編集の規則。c12af85 まで）を差分で取り込んだ。続けて b128439 までの変更（選んだ文字数の `CharacterCount`・
  英訳の既定のキーを Shift+Space・`documentContext`・`learningEnabled`・`privateModeHotkey`・`modelAdapterPath`・
  `selectionTriggerMode`・`selectionCharacterCount`）も取り込んだ。「こえ」モード（a5104ed）は取り込んでいない
- フロントエンドは **IBus**（GNOME の標準）と **fcitx5**（KDE など）。どちらもキーをセッション（`InputSession`）に送り、
  返ってきた表示を反映するだけで、入力の判断と状態はセッションにある（Windows版の TIP と同じ）
- IBus のエンジンは ibus-daemon が起動する別のプロセスなので、Windows版のように変換サーバとアプリ側の部品を分けず、
  エンジンのプロセスに変換エンジン・`SessionManager` を直接置く。fcitx5 のエンジンは fcitx5 本体のプロセスに読み込まれる
- 対象は x86_64・CPU 推論（llama.cpp は AVX2 まで。手元の CPU 向けの最適化は使わない）

## 構成

```
iroha-core/     変換エンジン（Windows版から写したもの）と Linux で足した部品
                  platform  データフォルダ・実行ファイルの場所・ホスト名・環境変数・設定の読み直しの依頼・入力中の目印
                  sha256    ダウンロードしたモデル・辞書の照合
shared/         OS の部品に頼る共有の部品（ライブラリ iroha-shared）
                  engine_host            設定からエンジン一式を組み立て、SessionManager に渡す（Windows版 iroha-server の
                                         ServerState / ServerBackend を移したもの。AI の依頼もここ）
                  ai_client              AI変換・AIで直す・選択テキストのAI編集の通信（libcurl。Windows版の WinHTTP を置き換え）
                  secret_store           API キーの保存（libsecret。main thread では呼ばない）
                  term_dictionary_fetcher / typo_normalizer_fetcher  専門辞書・打ち間違い訂正モデルの取得（libcurl）
                  emoji_renderability    描けない絵文字の判定（fontconfig で絵文字のフォントを探し、HarfBuzz で字形を数える）
                  launch_log             起動・終了・異常終了の記録（macOS版の LaunchLog）
                  key_shortcut.h         ショートカットの文字列（IBus / X のキーの値と照合する）
iroha-cli/      検証用 CLI（remote は無い。session はこのプロセスの中でセッションを動かす）
ibus-iroha/     IBus のエンジン（ibus-engine-iroha）
                  key_decision  キーの読み替え（IBus に依存しない。fcitx5 のエンジンと共有。Windows版 TIP の DecideKey と同じ規則）
                  engine        IBusEngine のサブクラス
                  main          エンジン一式を読み込んでから IBus につなぐ（--ibus なしで起動すると開発用に「iroha-dev」として登録する）
                  tests/        キーの読み替えの単体テストと、IBus を通して試す ibus_e2e.py
fcitx5-iroha/   fcitx5 のエンジン（アドオン iroha.so。fcitx5 の開発用ファイルがあるときだけ作る）
iroha-settings/ 設定画面（GTK4。Windows版の設定画面と同じタブ + 情報）
iroha-selection/ 選択テキストのAI編集の常駐プログラム（GTK4・X11。ログイン時に自動で起動する）
scripts/        build-llama.sh・fetch-model.sh・install.sh（開発用）・make-bundle.sh（配布物）・bundle/（配布物の中の install.sh など）
```

データフォルダは `IROHA_DATA_DIR` > `$XDG_DATA_HOME/iroha` > `~/.local/share/iroha`。中のファイル
（`config.json`・`models/`・`learning.json`・`user-dictionary.json`・`dictionaries/` など）の形式は macOS版・Windows版と同じ。
インストール先は `/usr/local/lib/iroha`（エンジン・設定画面・選択テキストのAI編集・`Dictionary`・`Symbols`）。
fcitx5 のエンジンは fcitx5 本体と同じ場所（`/usr/lib/x86_64-linux-gnu/fcitx5`・`/usr/share/fcitx5`）に入れ、辞書は
`/usr/local/lib/iroha` から読む（ビルド時に `IROHA_RESOURCE_DIR` として埋め込む）。

## ビルドと動作確認

```sh
# 要るパッケージ
sudo apt install cmake ninja-build libibus-1.0-dev libglib2.0-dev libgtk-4-dev libcurl4-openssl-dev \
    libsecret-1-dev libfontconfig-dev libfreetype-dev libharfbuzz-dev libx11-dev libxtst-dev libxi-dev libxfixes-dev libxext-dev
# fcitx5 のエンジンも作るなら
sudo apt install libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev

./linux/scripts/build-llama.sh          # llama.cpp を vendor/dist-linux へ（vendor/dist・dist-windows には触らない）
./macos/scripts/fetch-dictionary.sh     # 辞書ラティスの辞書（macOS版と同じコミット）
./macos/scripts/fetch-symbols.sh        # 記号・顔文字・絵文字のデータ（Mozc）
cmake -S linux -B linux/build -G Ninja && cmake --build linux/build
ctest --test-dir linux/build --output-on-failure

./linux/scripts/fetch-model.sh          # 既定のモデル iroha-kkc-v1 をデータフォルダへ（約82MB、SHA-256 を照合）

C=./linux/build/iroha-cli/iroha-cli
$C convert kyouhaiitenkidesune
$C session "kyouhaiitenkidesune<space><right><space><enter>"
$C symbols neko                         # 描けないとして除いた絵文字の数と例も出す
IROHA_LATTICE=off    $C ajimee testdata/ajimee/evaluation_items.json
IROHA_LATTICE=always $C ajimee testdata/ajimee/evaluation_items.json
```

`linux/build` は Dropbox の同期から外す（`python3 -c "import os; os.setxattr('linux/build','user.com.dropbox.ignored',b'1')"`）。
辞書・記号のデータを見つける順は Windows版と同じ（環境変数 `IROHA_DICTIONARY` / `IROHA_SYMBOLS` → 実行ファイルと同じ
フォルダ（fcitx5 はインストール先）の `Dictionary` / `Symbols` → カレントディレクトリから上へたどった `vendor/…`）。

### インストール

```sh
./linux/scripts/install.sh              # sudo。/usr/local と IBus のコンポーネントの定義。ibus-daemon の起動し直しまで行う
./linux/scripts/install.sh --uninstall
```

「設定 > キーボード > 入力ソース」で「日本語 (iroha)」を足すか、`ibus engine iroha` で切り替える。設定画面は入力ソースの
メニューの「設定…」か `/usr/local/lib/iroha/iroha-settings`。fcitx5 では「fcitx5 の設定」で入力メソッドに iroha を足す。

### 配布物

```sh
./linux/scripts/make-bundle.sh [版]     # dist/iroha-<版>-linux-x86_64.tar.gz
```

中身は `install.sh`・`uninstall.sh`・`fetch-model.sh`・`README.txt`・`files.txt`（入れるファイルの一覧）と、入れる先の形のまま
並べた `root/`。リリース（`.github/workflows/release.yml` の `linux` ジョブ）は、macOS版のジョブが Release を作ったあとに添付する。
**アセット名は `.tar.gz` にする**（macOS版の `UpdateChecker` は「iroha- で始まり .zip で終わる」最初のアセットを取るので、
`.zip` にすると macOS版のアップデータが Linux版を取りうる）。Linux版に自動アップデータは無い。
CI（`.github/workflows/ci.yml` の `build-and-test-linux`）は fcitx5 の開発用ファイルも入れてビルドとテストを行う。

## IBus のエンジン

```sh
# インストールせずに試す: 開発用のエンジンを引数なしで起動すると「iroha-dev」として登録する。確定は学習に残るので
# データフォルダを分ける
mkdir -p /tmp/iroha-e2e
IROHA_DATA_DIR=/tmp/iroha-e2e IROHA_MODEL=$HOME/.local/share/iroha/models/iroha-kkc-v1-Q5_K_M.gguf \
    ./linux/build/ibus-iroha/ibus-engine-iroha &
/usr/bin/python3 linux/ibus-iroha/tests/ibus_e2e.py "kyouhaiitenkidesune<space><right><space><down><enter>"
```

- **ibus_e2e.py はデスクトップで入力しているときに走らせない**。IBus 1.5 はエンジンをすべての入力コンテキストで共有するので、
  テストが入力中のアプリのエンジンを切り替え、終わったときに元のエンジンへ戻す（入力中に iroha へ切り替えていると外れる）。
  逆に、デスクトップでエンジンを切り替えるとテストの途中で合成が消える（2026-11-14 に起きた）
- **`ibus restart` を使わない**。GNOME（Ubuntu 22.04・IBus 1.5.26）が起動した ibus-daemon で使うと、止まったまま戻らず、
  デスクトップのアプリで入力できなくなった（2026-11-14）。install.sh は、動いている ibus-daemon と同じ引数・環境変数で
  `--replace --daemonize` を付けて起動し直す（`scripts/restart-ibus.sh`）
- 記録は `<データフォルダ>/logs/ibus-iroha-<ホスト名>.log`（1MB を超えたら起動のときに 1 世代残して作り直す）と、
  起動・終了の `launch-<ホスト名>.log`・`state-<ホスト名>.json`（前回の終了が無ければ unclean-exit。apport のレポートも拾う）
- 設定画面は `<データフォルダ>/reload-request` を書き換え、エンジンが見張って読み直す（Windows版の IPC の Reload にあたる）
- 表示の対応: 未確定文字列は下線、選んでいる文節は背景色。候補ウィンドウは IBus の候補表（今のページの 9 件だけを渡す。
  クリックの番号がページの中の番号になるように）。**候補の表（Tab）は IBus では表として描けない**ので、今の列だけを出し、
  補助テキストに「列 2/5（←→ で隣の列）」を出す。補助テキスト（候補の上の 1 行）には、知らせ・打ち間違いの訂正・AI の案・
  AIで直すの窓・AI変換の結果待ち・予測変換・候補の位置を、この順で 1 つだけ出す
- フォーカスが外れたときは、IBus が表示中の未確定文字列をそのまま確定する（`IBUS_ENGINE_PREEDIT_COMMIT`）。セッションは
  Reset で捨てる。ほかのエンジンに切り替えたとき（disable）は、今の表示のまま確定する
- 入力モード（あ / A）・プライベートモード・設定はプロパティ（GNOME の上のバーの入力ソースのメニュー）。英数モードは
  半角/全角キー・英数キー・Alt+` で切り替える。英数モードは入力コンテキストごとで、有効にするたびにかなに戻る
- 左文脈は周辺テキスト（surrounding text）から読む。確定の取り消し（Ctrl+Backspace）は周辺テキストでカーソルの手前を
  確かめてから `delete_surrounding_text` で消す
- どれかの入力コンテキストに未確定文字列がある間は `$XDG_RUNTIME_DIR/iroha/composing` に "1" を書く（選択テキストのAI編集が
  入力中のショートカットを無視するのに使う）

## fcitx5 のエンジン

- IBus のエンジンとの違い: フォーカスが外れた・リセットされた・ほかの入力メソッドに切り替えたときは、今の表示のまま確定する
  （fcitx5 は未確定文字列を自動では確定しない）。入力モード（あ / A）は入力メソッドの補助の表示（subModeLabel）。
  プライベートモードと設定はステータスの領域のメニュー。設定の読み直しの依頼は inotify で見張る
- このマシン（GPU マシン）には fcitx5 の開発用ファイルが入っていないので、`apt-get download` で取り出したヘッダに対して
  コンパイルと共有ライブラリのリンク（未解決の名前が無いこと）までを確かめた（2026-11-14）。**fcitx5 で動かす確認はしていない**。
  CI でビルドする

## 設定画面（iroha-settings）

`--tab model|input|dictionary|terms|rules|ai|selection|learning|about`。タブと項目は Windows版の設定画面と同じで、
情報タブ（版・データフォルダ・ライセンスの一覧）を足した。入力タブには、Windows版の設定画面に無い「区切り記号で囲んだ
英字入力」と「AIで直す」も置いた（記号・キーは config.json の `delimitedAlphabetStart` / `End`・`aiFixShortcut`）。
時間のかかる処理（取得と照合・AI のモデルの一覧・API キーの読み書き）は裏のスレッドで行う。保存したら `RequestReload()`。

## 選択テキストのAI編集（iroha-selection）

ほかのアプリで選んだ文章をショートカット（既定 Ctrl+Alt+1〜4、その場で指示する Ctrl+Alt+0）で AI に渡し、結果の窓から
元のアプリに貼って置き換える。設定で ON のときだけキーをつかむ。Windows版から変えたところ:

- 選択は X11 の PRIMARY 選択から読む（Ctrl+C を送らず、クリップボードを書き換えない）。結果は CLIPBOARD に置き、
  元の窓を前面に戻して XTest で Ctrl+V を送り、1 秒後に前のクリップボードの文字列を戻す
- 使わないアプリは、前面の窓の `_NET_WM_PID` の `/proc/<pid>/comm` と WM_CLASS で照らす
- Wayland では何もしない（グローバルショートカットが取れない）。端末では Ctrl+V で貼れないので置き換えは効かない
- マウスで選んだとき（ドラッグ・ダブルクリック。`selectionTriggerMode`、既定はアイコン）は、マウスの右下にアイコン
  （と選んだ文字数）を出し、押すと「AIに指示…」とプリセットのメニューにする（macOS版の SelectionBubbleController・
  SelectionMenuController）。「すぐにメニュー」「Alt を押しながら選んだときにメニュー」も選べる。選んだ文字数
  （`selectionCharacterCount`、既定 OFF）は AI編集が OFF でも出す
  - マウスは XInput2 の生のイベントで見る（つかまない）。**2.1 以上と名乗ること**（2.0 だと、押したアプリの暗黙のつかみの間の
    イベントが届かず、離したことが分からなかった）。左ボタンの位置は届いたときに `XQueryPointer` で読む
  - 選んだかは、押してから PRIMARY 選択の持ち主が変わったか（XFixes）で決める（窓を動かすドラッグでは変わらないので出さない）
  - アイコンとメニューはウィンドウマネージャに扱わせない窓（override-redirect）にする。押しても元のアプリのフォーカスと
    選択はそのまま。メニューで選んだら、選んでいた文章をショートカットのときと同じ流れで AI に渡す
  - 確かめ方: Xvfb（`apt-get download xvfb xdotool libxdo3 x11-apps` を `dpkg -x` で展開すれば sudo は要らない）に
    GTK4 の文章の窓と iroha-selection を出し、xdotool でドラッグ・ダブルクリックして xwd で画面を撮る。
    D-Bus は `dbus-run-session --config-file`（サービスの起動先を書かない設定）で分け、環境変数は `env -i` で空にしてから渡す
    （VS Code の snap の端末の GTK の環境変数が混ざると GTK のアプリが起動しない。portal が起動すると GTK の起動が 5 秒以上止まる）

## デバッグ表示（開発者モード）

macOS版の DeveloperOverlay を移したもの。設定 > 情報 > 開発者向けで「開発者モード」と「推論にかかった時間と左文脈を
カーソルの右下に表示する」を ON にすると（config.json の `developerMode`・`developerOverlay`。macOS版と同じキー）、
入力中にカーソルの右下へ次の行を出す（文面は macOS版と同じ）:

- 左文脈: 入力を始めたときに周辺テキストから読めたか（読めなければ代わりに使った確定文字列）
- かな漢字変換 1 回ごと: 種類（ライブ変換・文節に分ける・文節の区切り直し・候補ウィンドウ）・全体の時間・そのうち NN の
  時間と回数・読みの文字数・モデルの読み込み・結果を使わずに打ち切った数
- 打ち間違いの訂正 1 回ごと: きっかけ（入力の休止・スペース）・時間・結果

macOS版の「要素の文字」（アクセシビリティで要素の文字数を調べる）と加速度センサのグラフは無い。

- 行はコアが作る（`iroha/debug_overlay.h`。`SessionOutput::debugOverlay`）。NN の時間は `iroha/inference_timer.h` の
  スレッドごとの入れ物に、`ZenzEngine`・`TypoNormalizer` が計算の前後で足し込む（macOS版の InferenceTimer の
  タスクローカル値の代わり）。ライブ変換と休止での訂正は SessionManager の裏のスレッドで測る
- 窓は `shared/debug_overlay_window`（X11 の override-redirect の窓に Pango で描く。マウスは透過）。IBus は
  `set_cursor_location`、fcitx5 は `InputContext::cursorRect` の位置に出す。候補ウィンドウが出ているときは行の上に出す。
  Enter での確定・リセット（クリック・フォーカスの移動）で閉じる。Wayland では出さない
- 確かめ方: 利用者の IBus に触れないよう、Xvfb の上に別の D-Bus（`dbus-run-session --config-file`）・別の HOME・
  抽象ソケットのアドレス（`ibus-daemon -s -p disable -a unix:abstract=…`。ファイルのソケットは名前が 108 バイトを超えると
  切られる）で ibus-daemon を立て、開発用のエンジンと GTK3 の文章の窓を出して xdotool で打つ。GTK4 の窓は、ウィンドウ
  マネージャの無い Xvfb ではカーソルの位置を高さ 400 のように知らせてきたので確かめに使えなかった

## 確認したこと

2026-11-13〜14、Ubuntu 22.04・g++ 11.4・Xeon E5-1650 v4（12 スレッド）、iroha-kkc-v1:

| 評価 | Linux版 | Windows版の記録 |
|---|---|---|
| 単体テスト | コア 1191・共有の部品 20・キーの読み替え 79・設定画面 82・選択テキスト 79 件、すべて通る | — |
| 辞書ラティスの候補の並び（macOS版の出力との照合、AJIMEE 200 件） | n=10・n=30 とも 200/200 一致 | 200/200 一致 |
| AJIMEE モデルのみ / ラティス再採点 | 171 (85.5%) / 170 (85.0%) | 171 (85.5%) / 171 (85.5%) |
| eval.tsv 40 件 | 37 (92.5%) | 38 (95.0%) |
| 描けないとして除く絵文字 | 1,918 個のうち 8 個（Noto Color Emoji 2.047 に無い Unicode 16 以降のもの。国旗は描ける） | 294 個（国旗を含む） |

- IBus のエンジン（`ibus_e2e.py`、IBus 1.5.26）: ライブ変換・文節変換・候補の選択・確定、F7・Esc・Backspace、英数モード、
  Ctrl+S の前の確定、確定の取り消し、周辺テキストからの左文脈。インストールしたものを ibus-daemon が `--ibus` で起動して
  動くこと。ユーザがデスクトップのアプリで使って動いた（2026-11-14）
- CLI のセッション: 区切り記号の英字入力（kyouha_iPhone_wo → 今日はiPhoneを）・候補の表（Tab で広げて → で隣の列）
- 配布物（make-bundle.sh）が作れること（14MB・3,410 ファイル）
- デバッグ表示（Xvfb・別の ibus-daemon。2026-11-14）: 打っている間に左文脈とライブ変換の行、Space で候補ウィンドウの行
  （全体 130ms・NN 125ms（2回））、Enter で閉じる
- 選択テキストのマウスの操作（Xvfb・ウィンドウマネージャなし。2026-11-14）: ドラッグで「✦ 18文字」のアイコン、英単語の
  ダブルクリックで「1 word (4 chars)」、アイコンを押すとメニュー、メニューのプリセットで結果の窓（選択の文章つき）、
  何も選ばないドラッグでは出ない、「すぐにメニュー」「Alt を押しながら」「文字数だけ（AI編集 OFF）」の出し分け

**確かめていないこと**: AI変換・AIで直す・選択テキストのAI編集の実際の依頼と表示（接続先を設定していない）、設定画面
（画面を出していない）、ウィンドウマネージャ（GNOME）のあるデスクトップでのアイコン・メニュー・結果の窓・デバッグ表示、
fcitx5 でのデバッグ表示、libsecret での API キーの読み書き、設定画面からの読み直しが実際のエンジンに効くこと、
fcitx5 で動かすこと、配布物の install.sh（sudo が要る）、Wayland、IBus の候補表に出る補助テキストの見え方。

## これからの段階

1. [x] 段階 1: コアと iroha-cli
2. [x] 段階 2: IBus のエンジンと install.sh
3. [x] 段階 3: 設定画面（GTK4）・取得は libcurl
4. [x] 段階 4: 描けない絵文字の除外・起動の記録・AI変換・AIで直す・候補の表・区切り記号の英字入力・選択テキストのAI編集
5. [x] 段階 5: 配布物（tar.gz）・CI・リリースの Linux のジョブ
6. [x] 段階 6: fcitx5 のエンジン（ビルドまで）
7. [ ] 段階 7: 「こえ」モード（下の計画）
8. [ ] 実機での確認の残り（上の「確かめていないこと」）

## 「こえ」モード（計画。未実装）

macOS版（SpeechAnalyzer）・Windows版（SAPI、a5104ed）と同じく、決めたキーを押している間だけ録音し、離したら認識結果を
読みに戻して未確定文字列に入れる（確定はしない）。Linux には OS の日本語の音声認識が無いので、端末内で動く認識器を選ぶ。

- **コア**: Windows版の a5104ed のコアの変更（`VoiceTranscript`・`VoiceComparison`・`InputSession` の VoiceStart / VoiceStop /
  VoiceCancel と録音・認識の経過の受け取り・声で入れた合成を学習に残さない規則）を差分で取り込む。OS に依存しない
- **認識器の候補**（端末内・日本語・ストリーミングの途中結果）。先に実際の音声で比べてから決める:
  - sherpa-onnx + ReazonSpeech（k2 の日本語モデル。Apache-2.0）: ストリーミングの途中結果がある。ONNX Runtime に依存
  - Vosk（Kaldi。日本語の小さいモデル約 50MB。Apache-2.0）: 途中結果がある。精度は低めとみられる
  - whisper.cpp（MIT）: 精度は高いが途中結果は区切りごと。ggml を llama.cpp と別の版で持つので、同じプロセスに静的に
    入れると名前がぶつかる（別のプロセスにする）
- **読みの付け方**: 認識器が出すのは表記だけなので、表記を読みに戻す部分が要る（macOS版は CFStringTokenizer、Windows版は
  SAPI の語彙形）。MeCab + 辞書（mecab-ipadic-utf8 か UniDic）が候補。数字の直後の漢字（年・月・日・人）は表記のまま
  数字とつなげる規則（macOS版と同じ）は、取り込む `voice_transcript` に入っている
- **録音**: PipeWire / PulseAudio（libpulse-simple、16kHz・モノラル・16bit）。押してから 0.12 秒待って開き、0.35 秒未満なら
  捨てる（Windows版と同じ）
- **キー**: IBus・fcitx5 のエンジンは離したキーの知らせも受けるので、右 Ctrl（設定で変える）の押下と解放をセッションに送る
  （キーはアプリにも渡す）。認識は時間がかかるので、エンジンとは別のスレッド（whisper.cpp なら別のプロセス）で行う
- **確かめ方**: Windows版と同じく `iroha-cli voice <音声ファイル>` と、session の `<vstart>` `<vstop>`
