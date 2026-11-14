# 配布用のインストーラー iroha-<版>-windows-x64-setup.exe を作る（macos/scripts/make-bundle.sh の Windows版）。
#   powershell -ExecutionPolicy Bypass -File windows\scripts\make-installer.ps1 [-Version 0.18.0]
# 版は引数、無ければ git describe（v で始まるタグ）から取る。リリースは CI（.github/workflows/release.yml）がタグから渡す。
# 一式（TIP・変換サーバ・設定画面・辞書・記号のデータ・ライセンス）を %TEMP% に集め、Inno Setup（windows\installer\iroha.iss）で
# まとめる。出力先は windows\build\installer\。
# 要るもの: Inno Setup 6（ISCC.exe。winget install JRSoftware.InnoSetup、CI では choco install innosetup）

param([string]$Version = "")

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$BuildDir = Join-Path $RepoRoot "windows\build"

if ($Version -eq "") {
    $ErrorActionPreference = "Continue"
    $Version = (git -C $RepoRoot describe --tags --match "v[0-9]*" --always 2>$null)
    $ErrorActionPreference = "Stop"
    if (-not $Version) { $Version = "0.0.0-dev" }
}
$Version = "$Version" -replace "^v", ""

# Inno Setup のコンパイラ
$Iscc = $env:ISCC
if (-not $Iscc) {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    $Iscc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Iscc) { $Iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source }
}
if (-not $Iscc) { throw "Inno Setup 6 の ISCC.exe が見つかりません（winget install JRSoftware.InnoSetup）" }

# ビルド（テスト込み。版を実行ファイルに入れる）
& (Join-Path $PSScriptRoot "build-tip.ps1") -Version $Version

# 一式は %TEMP% に集める（リポジトリが Dropbox の下にあると、同期のファイルロックで失敗することがあるため）
$stage = Join-Path $env:TEMP "iroha-installer\iroha-$Version"
Remove-Item (Join-Path $env:TEMP "iroha-installer") -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item (Join-Path $BuildDir "iroha-tip\iroha-tip.dll") $stage
Copy-Item (Join-Path $BuildDir "iroha-server\iroha-server.exe") $stage
Copy-Item (Join-Path $BuildDir "iroha-settings\iroha-settings.exe") $stage
Copy-Item (Join-Path $RepoRoot "windows\resources\iroha.ico") $stage

# 辞書ラティスの辞書データ（azooKey_dictionary_storage、Apache-2.0）。LICENSE も一緒に置く
& (Join-Path $PSScriptRoot "fetch-dictionary.ps1")
$DictionaryRoot = Join-Path $RepoRoot "vendor\azooKey_dictionary_storage"
Copy-Item (Join-Path $DictionaryRoot "Dictionary") (Join-Path $stage "Dictionary") -Recurse
Copy-Item (Join-Path $DictionaryRoot "LICENSE") (Join-Path $stage "Dictionary\LICENSE")
# 記号・顔文字・絵文字の候補の Mozc のデータ（BSD 3-Clause）。BSD の条件で LICENSE も一緒に置く
& (Join-Path $PSScriptRoot "fetch-symbols.ps1")
$SymbolsRoot = Join-Path $RepoRoot "vendor\mozc-data"
New-Item -ItemType Directory -Force (Join-Path $stage "Symbols") | Out-Null
foreach ($file in @("symbol.tsv", "emoticon.tsv", "categorized.tsv", "emoji_data.tsv", "LICENSE")) {
    Copy-Item (Join-Path $SymbolsRoot $file) (Join-Path $stage "Symbols")
}
# ライセンスの全文（MIT は全文の同梱が条件）
$Licenses = Join-Path $stage "licenses"
New-Item -ItemType Directory -Force $Licenses | Out-Null
Copy-Item (Join-Path $RepoRoot "LICENSE") (Join-Path $Licenses "LICENSE-iroha.txt")
Copy-Item (Join-Path $RepoRoot "vendor\llama.cpp\LICENSE") (Join-Path $Licenses "LICENSE-llama.cpp.txt")
Copy-Item (Join-Path $RepoRoot "windows\iroha-core\src\lattice\LICENSE-AzooKeyKanaKanjiConverter.txt") $Licenses
Copy-Item (Join-Path $DictionaryRoot "LICENSE") (Join-Path $Licenses "LICENSE-azooKey_dictionary_storage.txt")
Copy-Item (Join-Path $SymbolsRoot "LICENSE") (Join-Path $Licenses "LICENSE-mozc.txt")

@"
iroha for Windows $Version
==========================

LLM（iroha-kkc-v1）でかな漢字変換する日本語入力です。
https://github.com/TetsuakiBaba/iroha

使い始める:
  設定 > 時刻と言語 > 言語と地域 > 日本語 > 言語のオプション > キーボード に「iroha」が現れます。
  Win+Space で切り替えます。設定画面は、スタートメニューの「iroha の設定」か、
  タスクバーの「あ」「A」をクリックしたメニューの「設定...」から開きます。
  このメニューで、ひらがな／半角英数の切り替え・ライブ変換・予測変換・プライベートモードも切り替えられます。

おもなキー:
  ローマ字で入力すると、そのまま変換されます（ライブ変換）。Enter で確定します。
  Space で文節ごとの変換（←→で文節を移る、Shift+←→で区切りを変える、Space・↑↓で候補）。
  Esc で読みに戻ります。半角/全角キー（または Alt+``）でかなと英数を切り替えます。
  AI変換（英訳）: 入力中に Shift+Space。AI の接続先は 設定 > AI で決めます。
  選択テキストのAI編集: 文章を選んで Ctrl+Alt+1〜4（プリセット）、Ctrl+Alt+0（その場で指示）。
  プライベートモード: Ctrl+Alt+P。

アップデート:
  1 日 1 回、新しい版があるかを確かめて知らせます（設定 > 情報 > アップデート）。

アンインストール:
  設定 > アプリ > インストールされているアプリ の「iroha」から。
  学習・ユーザ辞書・設定・変換モデル（%LOCALAPPDATA%\iroha）は残ります。

ライセンス（全文は licenses フォルダ）:
  iroha: MIT License
  変換モデル iroha-kkc-v1: CC BY-SA 4.0（https://github.com/TetsuakiBaba/iroha/releases/tag/iroha-kkc-v1）
  llama.cpp: MIT License
  AzooKeyKanaKanjiConverter（辞書ラティスの変換を C++ に移したもの）: MIT License
  辞書データ azooKey_dictionary_storage: Apache License 2.0
  記号・顔文字・絵文字のデータ（Mozc の src/data/、Google）: BSD 3-Clause License
"@ | Out-File (Join-Path $stage "README.txt") -Encoding utf8

# 出力も %TEMP% に作ってから移す（Dropbox の下に直接書くと、資源の書き込み（EndUpdateResource）が失敗する）
$TempOutput = Join-Path $env:TEMP "iroha-installer\output"
$Iss = Join-Path $RepoRoot "windows\installer\iroha.iss"
Write-Host "==> インストーラーを作る（$Iscc）"
& $Iscc /Q "/DAppVersion=$Version" "/DStageDir=$stage" "/DOutputDir=$TempOutput" $Iss
if ($LASTEXITCODE -ne 0) { throw "インストーラーを作れませんでした（ISCC: $LASTEXITCODE）" }

$OutputDir = Join-Path $BuildDir "installer"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$Installer = "iroha-$Version-windows-x64-setup.exe"
Move-Item (Join-Path $TempOutput $Installer) (Join-Path $OutputDir $Installer) -Force
Write-Host "==> 完了: $(Join-Path $OutputDir $Installer)"
