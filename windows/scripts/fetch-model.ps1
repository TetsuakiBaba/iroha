# zenz-v3.1-small (GGUF, Q5_K_M, 約72MB) をダウンロードする。
# macos/scripts/fetch-model.sh のWindows版。配置先: %LOCALAPPDATA%\iroha\models

$ErrorActionPreference = "Stop"

$ModelDir = Join-Path $env:LOCALAPPDATA "iroha\models"
$ModelFile = "zenz-v3.1-small-Q5_K_M.gguf"
$Url = "https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf/resolve/main/ggml-model-Q5_K_M.gguf"

$dest = Join-Path $ModelDir $ModelFile
if (Test-Path $dest) {
    Write-Host "既にダウンロード済み: $dest"
    return
}

New-Item -ItemType Directory -Force $ModelDir | Out-Null
Write-Host "==> zenz-v3.1-small をダウンロード中..."
$tmp = "$dest.tmp"
# Invoke-WebRequestは進捗表示が極端に遅いので無効化する
$ProgressPreference = "SilentlyContinue"
Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
Move-Item $tmp $dest -Force
Write-Host "==> 完了: $dest"
Write-Host "ライセンス: CC-BY-SA-4.0 (https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf)"
