# iroha 開発ガイド（Claude Code用）

## リリースポリシー（重要）

**ユーザーが明示的に「リリースして」と言うまでリリースしない。**
機能追加・修正はコミット + push + ローカルインストール（`./scripts/install.sh`）まで。
`git tag vX.Y.Z && git push origin vX.Y.Z` はリリース指示があったときのみ実行する
（タグpushでGitHub Actionsが署名・公証・Release作成まで自動実行される）。

## 日常の開発コマンド

```sh
swift build && swift test        # ビルドと単体テスト
./scripts/install.sh             # ローカルの ~/Library/Input Methods/ へインストール（ad-hoc署名）
/usr/bin/log stream --predicate 'process == "iroha"' --style compact  # IMEログ

# 設定ウィンドウだけを開く（IMEに接続しない。UI確認用）
"$HOME/Library/Input Methods/iroha.app/Contents/MacOS/iroha" --settings [input|dictionary|ai|model|about]
```

- `log` はzshの組み込みと衝突するため必ず `/usr/bin/log` をフルパスで呼ぶ
- NSLogはこの環境ではユニファイドログに残らない。IMEの実行時調査は
  /tmpへのファイル直書きヘルパーを一時的に仕込む（調査後に削除）

## プロジェクト構成の要点

- `Sources/iroha/` — IME本体（Swift 5モード）: IMKコントローラ、設定UI、
  AIバックエンド（Apple FoundationModels / Ollama / LM Studio / OpenAI互換）、
  アップデータ、モデルDL、macOSユーザ辞書の取り込み（`SystemUserDictionary`）、
  選択テキストのAI処理（`Selection/`。グローバルショートカットとマウス選択で
  他アプリの選択テキストをAIで置換する。GenGoの機能を移植。既定OFF・要アクセシビリティ権限）
- `Sources/IrohaCore/` — 変換エンジン（Swift 6モード）: zenz + llama.cpp
- FoundationModelsはmacOS 26+のため `#if canImport` + `@available(macOS 26.0, *)` ガード必須
  （パッケージのフロアはmacOS 14）
- llama.cppの静的ライブラリは `./scripts/build-llama.sh` で `vendor/dist` に生成（未コミット）
- ユーザ辞書・学習はLLMの外側で処理する（`ConversionEngine`のデコレータを
  学習 → ユーザ辞書 → LLM の順に重ね、読みを分割して一致部分を埋める）。
  macOS側の辞書は読み取り専用で絶対に書き込まない
- 学習は「文節変換の結果がエンジンの出力と違ったら記録」。同じ読みの使い分けのため
  文節ごとに直前の確定文字列を文脈として持つ（文脈なしの学習は入力の先頭でしか当てない）
- バージョンはgitタグが唯一の情報源。リリースはCIがタグから、開発ビルドは
  install.shがgit describeから注入する（Info.plistのコミット値はフォールバック。
  リリース時にゆるく追随させる）
