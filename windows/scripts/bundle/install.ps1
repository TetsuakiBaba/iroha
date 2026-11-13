# iroha for Windows のインストール（配布zip用。管理者PowerShellで実行）
#   1) ファイルを Program Files\iroha へ配置
#   2) TSFへ登録（regsvr32）
#   3) 変換サーバのログオン時自動起動を登録して起動
#   4) 変換モデル iroha-kkc-v1 をダウンロード（%LOCALAPPDATA%\iroha\models、約82MB）

$ErrorActionPreference = "Stop"

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "管理者権限で実行してください（TSF登録はHKLMへの書き込みが必要です）"
}

$SourceDir = $PSScriptRoot
$InstallDir = Join-Path $env:ProgramFiles "iroha"
$InstalledDll = Join-Path $InstallDir "iroha-tip.dll"

# 実行中のサーバを止める（exeの上書きのため）
try { Stop-Process -Name "iroha-server" -Force -ErrorAction Stop } catch {}

New-Item -ItemType Directory -Force $InstallDir | Out-Null

# ロード中のDLLは上書きできないがリネームはできるので、退避してから新DLLを置く
Get-ChildItem "$InstallDir\iroha-tip.dll.old-*" -ErrorAction SilentlyContinue | ForEach-Object {
    try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch {}
}
if (Test-Path $InstalledDll) {
    Move-Item $InstalledDll "$InstalledDll.old-$(Get-Date -Format yyyyMMddHHmmss)" -Force
}
Copy-Item (Join-Path $SourceDir "iroha-tip.dll") $InstalledDll
Copy-Item (Join-Path $SourceDir "iroha-server.exe") $InstallDir -Force
Copy-Item (Join-Path $SourceDir "iroha-settings.exe") $InstallDir -Force
# 辞書ラティスの辞書データ（変換サーバが実行ファイルと同じフォルダの Dictionary を読む）とライセンス文
foreach ($folder in @("Dictionary", "licenses")) {
    $installed = Join-Path $InstallDir $folder
    if (Test-Path $installed) { Remove-Item $installed -Recurse -Force }
    Copy-Item (Join-Path $SourceDir $folder) $installed -Recurse
}

Write-Host "==> TSF登録"
$p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/s", "`"$InstalledDll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 が失敗しました（コード: $($p.ExitCode)）" }

# サーバのログオン時自動起動（ストアアプリ内のTIPはサーバを起動できないため常駐前提）
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" `
    -Name "iroha-server" -Value "`"$(Join-Path $InstallDir 'iroha-server.exe')`""

Write-Host "==> 変換モデルの確認"
& (Join-Path $SourceDir "fetch-model.ps1")

# 管理者スクリプトから直接起動すると昇格権限を引き継ぐため、explorer.exe経由で
# デスクトップユーザ権限（非昇格）で起動する
Start-Process explorer.exe -ArgumentList "`"$(Join-Path $InstallDir 'iroha-server.exe')`""

Write-Host ""
Write-Host "==> インストール完了"
Write-Host "設定 > 時刻と言語 > 言語と地域 > 日本語 > 言語のオプション > キーボード に「iroha」が現れます。"
Write-Host "現れない場合はサインアウト/サインインしてください。Win+Space で切り替えられます。"
