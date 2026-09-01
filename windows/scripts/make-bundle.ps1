# 配布用zipを作る（macos/scripts/make-bundle.sh のWindows版）。
#   iroha-<version>-windows.zip
#     ├─ iroha-tip.dll / iroha-server.exe
#     ├─ install.ps1 / uninstall.ps1 / fetch-model.ps1
#     └─ README.txt
# バージョンは引数、無ければ git describe から取る（リリースはCIがタグから注入する）。

param([string]$Version = "")

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$BuildDir = Join-Path $RepoRoot "windows\build"

if ($Version -eq "") {
    $ErrorActionPreference = "Continue"
    $Version = (git -C $RepoRoot describe --tags --always 2>$null)
    $ErrorActionPreference = "Stop"
    if (-not $Version) { $Version = "dev" }
    $Version = "$Version" -replace "^v", ""
}

# ビルド（テスト込み）
& (Join-Path $PSScriptRoot "build-tip.ps1")

# ステージングは%TEMP%で行う（リポジトリがDropbox同期下にあると
# 同期のファイルロックでCompress-Archiveが失敗することがあるため）
$stageName = "iroha-$Version-windows"
$stage = Join-Path $env:TEMP "iroha-bundle\$stageName"
Remove-Item (Join-Path $env:TEMP "iroha-bundle") -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $BuildDir "iroha-tip\iroha-tip.dll") $stage
Copy-Item (Join-Path $BuildDir "iroha-server\iroha-server.exe") $stage
Copy-Item (Join-Path $BuildDir "iroha-settings\iroha-settings.exe") $stage
Copy-Item (Join-Path $PSScriptRoot "bundle\install.ps1") $stage
Copy-Item (Join-Path $PSScriptRoot "bundle\uninstall.ps1") $stage
Copy-Item (Join-Path $PSScriptRoot "fetch-model.ps1") $stage

@"
iroha for Windows $Version
==========================

LLM（zenzモデル）でかな漢字変換する日本語IMEです。

インストール:
  install.ps1 を右クリック →「PowerShellで実行」…ではなく、
  管理者PowerShellを開いて次を実行してください:

    powershell -ExecutionPolicy Bypass -File .\install.ps1

  初回はzenzモデル（約72MB）をダウンロードします。
  設定 > 時刻と言語 > 言語と地域 > 日本語 > キーボード に「iroha」が
  追加され、Win+Space で切り替えられます。

使い方:
  ローマ字入力 → Space で変換 → ←→で文節移動、Shift+←→で区切り調整、
  Space/↑↓で候補、数字キーで選択、Enter で確定、Esc で読みに戻る。
  半角/全角キー（または Alt+`）でかな⇔英数。

アンインストール:
    powershell -ExecutionPolicy Bypass -File .\uninstall.ps1
  学習・辞書・モデルも消す場合は -PurgeData を付けます。

ライセンス:
  iroha: https://github.com/TetsuakiBaba/iroha
  zenz-v3.1-small: CC-BY-SA-4.0 (Miwa-Keita/zenz-v3.1-small-gguf)
  llama.cpp: MIT (ggml-org/llama.cpp)
"@ | Out-File (Join-Path $stage "README.txt") -Encoding utf8

$zipTemp = Join-Path $env:TEMP "iroha-bundle\$stageName.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zipTemp
$zipPath = Join-Path $BuildDir "$stageName.zip"
Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
Move-Item $zipTemp $zipPath

Write-Host "==> 完了: $zipPath"
