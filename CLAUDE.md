# iroha 開発ガイド（Claude Code用）

## リリースポリシー（重要）

**ユーザーが明示的に「リリースして」と言うまでリリースしない。**
機能追加・修正はコミット + push + ローカルインストール（`./macos/scripts/install.sh`）まで。
`git tag vX.Y.Z && git push origin vX.Y.Z` はリリース指示があったときのみ実行する
（タグpushでGitHub Actionsが署名・公証・Release作成まで自動実行される）。

### 両プラットフォームのリリース方針（決定済み・Windows初回リリース時に実装）

- バージョンは macOS / Windows で**単一の系列（`vX.Y.Z`タグ）を共有**する。
  プラットフォーム別タグ（`macos-v…` 等）は作らない
- 1つのタグpushで両OSをビルドし、**1つのGitHub Releaseに両方の成果物を添付**する
  （`iroha-X.Y.Z-macos.zip` / `iroha-X.Y.Z-windows.zip` のようにアセット名にOSを入れる）
- release.ymlは「draft作成 → 各OSジョブがアップロード → 全部揃ったらpublish」の順にする
  （片方だけ公開されるとアップデータが自分のOSのアセットを見つけられないため）
- **注意**: アセット名を `iroha-X.Y.Z.zip` から変更する際は、配布済みmacOS版の
  `UpdateChecker.swift` が旧名を前提にしていないか先に確認する（壊れる場合は
  新旧両名でアップロードする移行リリースを挟む）
- `-` を含むタグ（例 `v0.6.0-beta.1`）はプレリリース＝アップデート通知対象外。
  Windows版が未成熟な間のベータ配布はこれを使う

## 日常の開発コマンド

```sh
cd macos && swift build && swift test   # ビルドと単体テスト（必ず macos/ から実行。
                                        # Package.swiftのvendor/dist参照がcwd相対のため）
./macos/scripts/install.sh       # ローカルの ~/Library/Input Methods/ へインストール（ad-hoc署名）
/usr/bin/log stream --predicate 'process == "iroha"' --style compact  # IMEログ

# 設定ウィンドウだけを開く（IMEに接続しない。UI確認用）
"$HOME/Library/Input Methods/iroha.app/Contents/MacOS/iroha" --settings [input|dictionary|ai|model|about]
```

- `log` はzshの組み込みと衝突するため必ず `/usr/bin/log` をフルパスで呼ぶ
- NSLogはこの環境ではユニファイドログに残らない。IMEの実行時調査は
  /tmpへのファイル直書きヘルパーを一時的に仕込む（調査後に削除）

## プロジェクト構成の要点

- プラットフォーム別レイアウト: macOS版のSwiftパッケージ一式（Package.swift / Sources /
  Tests / Resources / scripts）は `macos/` 配下。Windows版は今後 `windows/` に実装する。
  `vendor/`（llama.cpp）・`patches/`・`testdata/`・`training/`・`.venv` はプラットフォーム共有の
  ためリポジトリ直下に置く（**`training/` のスクリプトがルート直下の `vendor/`・`.venv` を
  参照しているので、これらを `macos/` 配下へ移動してはならない**）
- `macos/Sources/iroha/` — IME本体（Swift 5モード）: IMKコントローラ、設定UI、
  AIバックエンド（Apple FoundationModels / Ollama / LM Studio / OpenAI互換）、
  アップデータ、モデルDL、macOSユーザ辞書の取り込み（`SystemUserDictionary`）、
  選択テキストのAI編集（`Selection/`。グローバルショートカットとマウス選択で
  他アプリの選択テキストをAIで置換する。GenGoの機能を移植。既定OFF・要アクセシビリティ権限）
- `macos/Sources/IrohaCore/` — 変換エンジン（Swift 6モード）: zenz + llama.cpp。
  Foundationのみ依存でmacOS専用APIは不使用（将来のWindows移植候補）
- FoundationModelsはmacOS 26+のため `#if canImport` + `@available(macOS 26.0, *)` ガード必須
  （パッケージのフロアはmacOS 14）
- llama.cppの静的ライブラリは `./macos/scripts/build-llama.sh` で `vendor/dist` に生成（未コミット）
- ユーザ辞書・学習はLLMの外側で処理する（`ConversionEngine`のデコレータを
  学習 → ユーザ辞書 → LLM の順に重ね、読みを分割して一致部分を埋める）。
  macOS側の辞書は読み取り専用で絶対に書き込まない
- 学習は「文節変換の結果がエンジンの出力と違ったら記録」。同じ読みの使い分けのため
  文節ごとに直前の確定文字列を文脈として持つ（文脈なしの学習は入力の先頭でしか当てない）
- バージョンはgitタグが唯一の情報源。リリースはCIがタグから、開発ビルドは
  install.shがgit describeから注入する（Info.plistのコミット値はフォールバック。
  リリース時にゆるく追随させる）。Windows版も同じ原則でCIがタグから注入すること

## Windows版の開発（windows/、Windowsマシン上のClaude向け）

- WindowsのIMEはTSF（Text Services Framework）ベースで `windows/` に実装する。
  macOSのIMK部分（`macos/Sources/iroha/`）は流用不可
- 変換エンジンは選択肢が2つ: `macos/Sources/IrohaCore/` をSwift for Windowsで共有
  （Foundation + llama.cppのみ依存で移植可能な設計）、または別言語で再実装。
  **方針は未決定。実装前にユーザーに確認する**
- llama.cppのソースはルートの `vendor/llama.cpp` を共有（zenz対応パッチ
  `patches/llama-cpp-zenz-pretokenizer.patch` の適用が必要）。ただし
  `vendor/dist` はmacOS用（Metal依存）なので**Windows用は別ディレクトリ
  （例 `vendor/dist-windows`）にビルドし、`vendor/dist` を上書きしない**
- 変換の仕様（zenzのプロンプト形式・読み制約・学習/ユーザ辞書のデコレータ構成）は
  README.mdの「開発」節と `macos/Sources/IrohaCore/` の実装が正。挙動を合わせること
- GGUFモデル・評価データ（`testdata/`）・学習パイプライン（`training/`）は
  プラットフォーム非依存でそのまま使う。`training/` は学習作業中のことがあるので
  **明示的な指示なしに変更しない**
- リリースは上記「両プラットフォームのリリース方針」に従う（単一タグ・単一Release）
