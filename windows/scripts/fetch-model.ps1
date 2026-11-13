﻿# 既定の変換モデル iroha-kkc-v1 (GGUF, Q5_K_M, 約82MB) をダウンロードする。
# macos/scripts/fetch-model.sh のWindows版（同じ配布元・同じ照合値。モデルを差し替えるときは
# macos/Sources/iroha/ModelDownloader.swift・fetch-model.sh・iroha-core の config.h と合わせる）。
# 配置先: %LOCALAPPDATA%\iroha\models
#
# -Model zenz-v3.1-small で、以前の既定のモデル（比較用）を取得する

param([ValidateSet("iroha-kkc-v1", "zenz-v3.1-small")][string]$Model = "iroha-kkc-v1")

$ErrorActionPreference = "Stop"

$ModelDir = Join-Path $env:LOCALAPPDATA "iroha\models"
if ($Model -eq "iroha-kkc-v1") {
    $ModelFile = "iroha-kkc-v1-Q5_K_M.gguf"
    $Url = "https://github.com/TetsuakiBaba/iroha/releases/download/iroha-kkc-v1/iroha-kkc-v1-Q5_K_M.gguf"
    $Sha256 = "f060e259dbed28754f71e1634bc01a5d9d735d519f8ff72c333cac77c859efda"
    $License = "CC BY-SA 4.0 (https://github.com/TetsuakiBaba/iroha/releases/tag/iroha-kkc-v1)"
} else {
    $ModelFile = "zenz-v3.1-small-Q5_K_M.gguf"
    $Url = "https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf/resolve/main/ggml-model-Q5_K_M.gguf"
    $Sha256 = ""
    $License = "CC-BY-SA-4.0 (https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)"
}

$dest = Join-Path $ModelDir $ModelFile
if (Test-Path $dest) {
    Write-Host "既にダウンロード済み: $dest"
    return
}

New-Item -ItemType Directory -Force $ModelDir | Out-Null
Write-Host "==> $Model をダウンロード中..."
$tmp = "$dest.tmp"
# Invoke-WebRequestは進捗表示が極端に遅いので無効化する
$ProgressPreference = "SilentlyContinue"
Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
# 途中で切れた・差し替わったファイルを置かない
if ($Sha256 -ne "" -and (Get-FileHash $tmp -Algorithm SHA256).Hash -ne $Sha256) {
    Remove-Item $tmp -Force
    throw "ダウンロードしたファイルの SHA-256 が一致しません"
}
Move-Item $tmp $dest -Force
Write-Host "==> 完了: $dest"
Write-Host "ライセンス: $License"
