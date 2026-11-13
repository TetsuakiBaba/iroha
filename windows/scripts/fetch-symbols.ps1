# 記号・顔文字・絵文字の候補に使う Mozc のデータ（BSD 3-Clause、Google）を vendor/ に取得する。
# macos/scripts/fetch-symbols.sh のWindows版。SymbolCandidateTable（iroha-core）が読む TSV で、
# install-tip.ps1・make-bundle.ps1 が実行ファイルと同じフォルダの Symbols へコピーする。
# macOS版と同じコミットを使う（コミットを変えるときは両方のスクリプトを合わせる）。
#
# 取得するのは Mozc の src/data/ のうち Google が書いたデータだけ（README の License 節で
# 第三者のデータとされている dictionary_oss/ は使わない）。
#
# 注意: vendor/mozc-data はDropboxでmacOSマシンと共有している。
#       別のコミットが既に置いてあるときは、消して取り直すことはせず止まる（-Force で入れ替える）。
#
# EAPをContinueにする理由はbuild-llama.ps1のコメントを参照
param([switch]$Force)

$ErrorActionPreference = "Continue"

$MozcCommit = "4f25784ad1bcab1bbfe7cc2c4a22e32293fd6a48"   # 2026-10-08
$Files = @(
    "src/data/symbol/symbol.tsv",
    "src/data/emoticon/emoticon.tsv",
    "src/data/emoticon/categorized.tsv",
    "src/data/emoji/emoji_data.tsv",
    "LICENSE"
)

# vendor/ はリポジトリルートにある
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Dest     = Join-Path $RepoRoot "vendor\mozc-data"
$Short    = $MozcCommit.Substring(0, 7)
$CommitFile = Join-Path $Dest "COMMIT"

if (Test-Path $CommitFile) {
    $current = (Get-Content $CommitFile -Raw).Trim()
    if ($current -eq $MozcCommit) {
        Write-Host "==> Mozc の記号データは取得済み ($Dest @ $Short)"
        exit 0
    }
    if (-not $Force) {
        throw "$Dest に別のコミットのデータがあります（$current）。macOS版と共有しているフォルダなので入れ替えません。入れ替えるときは -Force を付けてください"
    }
}

Write-Host "==> Mozc の記号データを取得 ($Short)"
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Force $Dest | Out-Null
# Windows PowerShell 5.1 の既定は TLS 1.0 のことがあるので 1.2 を有効にする
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
foreach ($path in $Files) {
    # emoticon.tsv と categorized.tsv は名前で区別できるので、ディレクトリは平らにする
    $out = Join-Path $Dest (Split-Path $path -Leaf)
    $url = "https://raw.githubusercontent.com/google/mozc/$MozcCommit/$path"
    $ok = $false
    for ($attempt = 1; $attempt -le 3 -and -not $ok; $attempt++) {
        try {
            Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -ErrorAction Stop
            $ok = $true
        } catch {
            if ($attempt -eq 3) { throw "取得に失敗しました: $url ($_)" }
            Start-Sleep -Seconds 2
        }
    }
}
# macOS版（echo）と同じく末尾に LF を 1 つ付ける
[IO.File]::WriteAllText($CommitFile, "$MozcCommit`n")
Write-Host "==> 完了: $Dest"
