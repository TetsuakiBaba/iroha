# AJIMEE-Bench（zenzai公式のかな漢字変換ベンチマーク）の評価データを取得する。
# 出典: https://github.com/azooKey/AJIMEE-Bench （JWTD_v2/v1、200件）
# macos/scripts/fetch-ajimee.sh のWindows版。

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Dest = Join-Path $RepoRoot "testdata\ajimee\evaluation_items.json"

New-Item -ItemType Directory -Force (Split-Path $Dest) | Out-Null
$ProgressPreference = "SilentlyContinue"
Invoke-WebRequest -UseBasicParsing -OutFile $Dest `
    "https://raw.githubusercontent.com/azooKey/AJIMEE-Bench/main/JWTD_v2/v1/evaluation_items.json"
Write-Host "wrote $Dest"
