# iroha for Windows のアンインストール（管理者PowerShellで実行）。
# 学習・ユーザ辞書・モデル（%LOCALAPPDATA%\iroha）は既定では残す。
# すべて消す場合は -PurgeData を付けて実行する。

param([switch]$PurgeData)

$ErrorActionPreference = "Stop"

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "管理者権限で実行してください"
}

$InstallDir = Join-Path $env:ProgramFiles "iroha"
$InstalledDll = Join-Path $InstallDir "iroha-tip.dll"

try { Stop-Process -Name "iroha-server" -Force -ErrorAction Stop } catch {}
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" `
    -Name "iroha-server" -ErrorAction SilentlyContinue

if (Test-Path $InstalledDll) {
    Write-Host "==> TSF登録を解除"
    $p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/u", "/s", "`"$InstalledDll`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { Write-Warning "regsvr32 /u が失敗しました（コード: $($p.ExitCode)）" }
}
# DLLが他プロセスに掴まれていても消せるものは消す（残りはサインアウト後に消える）
Get-ChildItem $InstallDir -ErrorAction SilentlyContinue | ForEach-Object {
    try { Remove-Item $_.FullName -Force -Recurse -ErrorAction Stop } catch {}
}
if ((Test-Path $InstallDir) -and -not (Get-ChildItem $InstallDir)) {
    Remove-Item $InstallDir -Force
}

if ($PurgeData) {
    Remove-Item (Join-Path $env:LOCALAPPDATA "iroha") -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "==> 学習・辞書・モデルも削除しました"
}

Write-Host "==> アンインストール完了（完全に反映するにはサインアウトしてください）"
