# iroha（いろは）

ローカルLLMをかな漢字変換エンジンとして用いるmacOS用日本語IME。

- **ライブ変換**: ことえりのライブ変換のように、入力に追従して読みをLLMへ送り、変換結果をリアルタイム表示。Enterで確定
- **候補変換**: スペースキーでn-best候補を候補ウィンドウに表示
- **文脈対応**: 直前に確定した文字列を条件としてLLMに与え、文脈に合った変換を行う
- **ローカル動作**: 変換モデルは [zenz-v3.1-small](https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)（GPT-2系95M・GGUF・約70MB）を llama.cpp（Metal）で実行。実測レイテンシは1変換あたり15〜30ms程度（Apple Silicon）

## インストール

- macOS 14以降 / Apple Silicon（リリースビルドは arm64 のみ）

1. [Releases](https://github.com/TetsuakiBaba/iroha/releases/latest) から `iroha-x.y.z.zip` をダウンロードして解凍
2. `iroha.app` をダブルクリック — 自動で `~/Library/Input Methods/` にインストール・登録される
3. **システム設定 > キーボード > 入力ソース > 編集 > + > 日本語 > iroha** を追加
   （一覧に出ない場合は一度ログアウト/ログインする）

変換モデル [zenz-v3.1-small](https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)（約72MB、CC-BY-SA-4.0）は初回起動時に自動ダウンロードされる。
アップデートは新バージョン公開時に自動で通知される（メニューバーの入力ソースアイコン >
「アップデートを確認...」で手動確認も可能）。

## ソースからのビルド

- macOS 14以降（Apple Silicon推奨）
- Xcode（Swiftツールチェーン）
- CMake（`brew install cmake`、llama.cppのビルドに使用）

```sh
# 1. llama.cppをスタティックビルド（初回のみ、zenz対応パッチを適用）
./scripts/build-llama.sh

# 2. 変換モデル(zenz-v3.1-small)をダウンロード（初回のみ、約70MB）
./scripts/fetch-model.sh

# 3. ビルドして ~/Library/Input Methods/ にインストール
./scripts/install.sh
```

その後、**システム設定 > キーボード > 入力ソース > 編集 > + > 日本語 > iroha** を追加する
（一覧に出ない場合は一度ログアウト/ログインする）。

## 使い方

| キー | 動作 |
|---|---|
| ローマ字入力 | かなに変換され、続けてLLMがライブ変換 |
| Enter | 表示中の変換結果を確定 |
| Space | 文節変換モードへ（文節に分割して表示、もう一度Spaceで候補ウィンドウ） |
| ← / → | 文節間を移動（文節変換中、選択中の文節は太い下線） |
| Shift+← / Shift+→ | 選択中の文節の区切りを伸縮（以降の文節は自動で再変換） |
| ↑↓ / Space | 候補ウィンドウ内の選択（Enterで採用、Escで閉じる） |
| Esc | 候補→文節→かな→取消 と段階的に戻る |
| Backspace | 1文字削除（文節変換中はかな入力に戻る） |
| 、。！？ | 自動確定（メニューでOFF可、ライブ変換時のみ） |
| F6 / F7 / F8 | ひらがな / カタカナ / 半角カタカナ（Ctrl+U / I / O でも可） |
| F9 / F10 | 全角英数 / 半角英数（打鍵通りの文字列、Ctrl+P / T でも可） |
| 英数 / かな | 英数モード / ひらがなモード切替（JISキーボード） |
| Ctrl+Shift+J / ; | ひらがな / 英数モード切替 |

文節分割は「変換結果中のひらがな（助詞・送り仮名）を読みと突き合わせる」軽量な
アライメント（[ReadingAligner](Sources/IrohaCore/ReadingAligner.swift)）による初期推定で、
Shift+←→でユーザがいつでも調整できる。文節の候補生成は選択中の文節の読みと
左側の確定済み文字列を文脈としてLLMに与えて行う。

メニューバーの入力ソースアイコンのメニューの「設定...」から設定ウィンドウを開ける:
ライブ変換・句読点で自動確定・候補数・句読点スタイル（、。/ ，．）・モデルパスの変更。
パスワード欄（Secure Input）ではmacOSがIMEをシステムレベルで無効化するため、
iroha側の対応は不要。

## 開発

```sh
swift test                                 # 単体テスト（ローマ字変換など）
swift build && .build/debug/iroha-cli repl  # CLIで変換を試す（レイテンシ表示付き）
.build/debug/iroha-cli convert --n 5 "きしゃ"          # n-best候補
.build/debug/iroha-cli convert --context "天気予報によると" "きしょう"  # 文脈条件付け
log stream --predicate 'process == "iroha"' --style compact  # IMEのログ
```

- 変換エンジンは [ConversionEngine](Sources/IrohaCore/ConversionEngine.swift) プロトコルで抽象化されており、
  [ZenzEngine](Sources/IrohaCore/ZenzEngine.swift)（zenz-v3 + llama.cpp）を別モデルに差し替えられる
- モデルの評価は `iroha-cli bench testdata/eval.tsv`（完全一致率・CER・レイテンシ）。
  自作モデルの学習パイプライン（データ準備→学習→GGUF変換→評価）は [training/](training/README.md) を参照
- モデルファイルは `~/Library/Application Support/iroha/models/` に置く（環境変数 `IROHA_MODEL` で上書き可）
- zenzのプロンプト形式: `[U+EE02 + 左文脈] + U+EE00 + カタカナ読み + U+EE01 → 変換結果`
- llama.cppにはzenzのpre-tokenizer名（`gpt2-small-japanese-char`）を認識させる
  [パッチ](patches/llama-cpp-zenz-pretokenizer.patch)を当てている（build-llama.shが自動適用）

## リリース手順（メンテナ向け）

バージョンは **gitタグが唯一の情報源**。タグをpushすると
[release.yml](.github/workflows/release.yml) がビルド→Developer ID署名→公証→GitHub Release作成まで自動実行する:

```sh
git tag v0.4.0 && git push origin v0.4.0
```

- `CFBundleShortVersionString` はタグから、`CFBundleVersion` はワークフローの実行番号から注入される
  （`Resources/Info.plist` のコミット値は開発ビルドの表示用）
- `-` を含むタグ（例 `v0.4.0-beta.1`）はプレリリースになり、アップデート通知の対象外
  （アップデータのQAは `defaults write dev.iroha.inputmethod.iroha updateCheckURL <リリースAPIのURL>` で行う）
- 署名関連のSecrets: `MACOS_CERTIFICATE_P12` / `MACOS_CERTIFICATE_PASSWORD` / `MACOS_SIGN_IDENTITY` /
  `APPLE_TEAM_ID` / `NOTARY_KEY_ID` / `NOTARY_ISSUER_ID` / `NOTARY_KEY_P8`。
  証明書を更新した場合、Team IDが変わると `UpdateChecker.swift` の `expectedTeamID` も更新が必要
  （不一致だとセルフアップデートの署名検証が通らなくなる）

## ライセンスと帰属

- 本リポジトリのコード: [MIT](LICENSE)
- 変換モデル [zenz-v3.1](https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)（Keita Miwa氏）: CC-BY-SA-4.0
- [llama.cpp](https://github.com/ggml-org/llama.cpp): MIT
- 設計にあたり [azooKey-Desktop](https://github.com/azooKey/azooKey-Desktop) / Zenzai の公開知見を参考にした
