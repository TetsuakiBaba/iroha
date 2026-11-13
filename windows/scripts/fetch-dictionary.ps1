# azooKeyの辞書データ（azooKey_dictionary_storage、Apache-2.0）を vendor/ に取得する。
# macos/scripts/fetch-dictionary.sh のWindows版。辞書ラティス（iroha-core の src/lattice/）が読む
# LOUDS 形式の辞書で、macOS版と同じコミットを使う（コミットを変えるときは両方のスクリプトを合わせる）。
#
# 注意: vendor/azooKey_dictionary_storage はDropboxでmacOSマシンと共有している。
#       別のコミットが既に置いてあるときは、消して取り直すことはせず止まる
#       （macOS版が使っている辞書をWindows側から入れ替えないため。-Force で入れ替える）。
#
# EAPをContinueにする理由はbuild-llama.ps1のコメントを参照
param([switch]$Force)

$ErrorActionPreference = "Continue"

$DictCommit = "832fbb0d3039dfaa4b2183956f3d96f6b07eec4d"   # AzooKeyKanaKanjiConverter v0.11.2 が参照

# vendor/ はリポジトリルートにある
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Dest     = Join-Path $RepoRoot "vendor\azooKey_dictionary_storage"
$Short    = $DictCommit.Substring(0, 7)

if (Test-Path (Join-Path $Dest "Dictionary\mm.binary")) {
    $current = git -C $Dest rev-parse HEAD 2>$null
    if ($current -eq $DictCommit) {
        Write-Host "==> 辞書データは取得済み ($Dest @ $Short)"
        exit 0
    }
    if (-not $Force) {
        throw "$Dest に別のコミットの辞書があります（$current）。macOS版と共有しているフォルダなので入れ替えません。入れ替えるときは -Force を付けてください"
    }
}

Write-Host "==> azooKey辞書データを取得 ($Short)"
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Force $Dest | Out-Null
git -C $Dest init --quiet
if ($LASTEXITCODE -ne 0) { throw "git init に失敗しました" }
git -C $Dest remote add origin https://github.com/azooKey/azooKey_dictionary_storage
git -C $Dest fetch --quiet --depth 1 origin $DictCommit
if ($LASTEXITCODE -ne 0) { throw "git fetch に失敗しました" }
git -C $Dest -c advice.detachedHead=false checkout --quiet FETCH_HEAD
if ($LASTEXITCODE -ne 0) { throw "git checkout に失敗しました" }
Write-Host "==> 完了: $(Join-Path $Dest 'Dictionary')"
