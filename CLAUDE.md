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
```

- `log` はzshの組み込みと衝突するため必ず `/usr/bin/log` をフルパスで呼ぶ
- NSLogはこの環境ではユニファイドログに残らない。IMEの実行時調査は
  /tmpへのファイル直書きヘルパーを一時的に仕込む（調査後に削除）

## プロジェクト構成の要点

- `Sources/iroha/` — IME本体（Swift 5モード）: IMKコントローラ、設定UI、
  翻訳（Apple FoundationModels / Ollama / LM Studio）、アップデータ、モデルDL、
  macOSユーザ辞書の取り込み（`SystemUserDictionary`）
- `Sources/IrohaCore/` — 変換エンジン（Swift 6モード）: zenz + llama.cpp
- FoundationModelsはmacOS 26+のため `#if canImport` + `@available(macOS 26.0, *)` ガード必須
  （パッケージのフロアはmacOS 14）
- llama.cppの静的ライブラリは `./scripts/build-llama.sh` で `vendor/dist` に生成（未コミット）
- ユーザ辞書はLLMの外側で処理する（`UserDictionaryEngine`が読みを分割し、辞書一致部分を
  埋めて残りだけをエンジンに渡す）。macOS側の辞書は読み取り専用で絶対に書き込まない
- バージョンはgitタグが唯一の情報源。リリースはCIがタグから、開発ビルドは
  install.shがgit describeから注入する（Info.plistのコミット値はフォールバック。
  リリース時にゆるく追随させる）
