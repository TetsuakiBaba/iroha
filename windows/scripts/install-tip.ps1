# iroha-tip.dll を Program Files に配置してTSFに登録する（要・管理者権限）。
#   インストール:   管理者PowerShellで ./install-tip.ps1
#   アンインストール: 管理者PowerShellで ./install-tip.ps1 -Uninstall
#
# 注意: DLLは入力先の各プロセスにロードされるため、差し替え時は
# 「MS-IMEに切り替え → 対象アプリを終了」してから実行する。
# それでもロックされている場合はサインアウト/再起動が必要。

param([switch]$Uninstall)

$ErrorActionPreference = "Stop"

$identity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $identity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "管理者権限で実行してください（TSF登録はHKLMへの書き込みが必要です）"
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$BuiltDll = Join-Path $RepoRoot "windows\build\iroha-tip\iroha-tip.dll"
$BuiltServer = Join-Path $RepoRoot "windows\build\iroha-server\iroha-server.exe"
$InstallDir = Join-Path $env:ProgramFiles "iroha"
$InstalledDll = Join-Path $InstallDir "iroha-tip.dll"

# 変換サーバが動いていると exe を上書きできないため先に止める
try { Stop-Process -Name "iroha-server" -Force -ErrorAction Stop } catch {}

if ($Uninstall) {
    if (Test-Path $InstalledDll) {
        Write-Host "==> TSF登録を解除"
        # regsvr32はGUIアプリなのでStart-Process -Waitで終了を待って終了コードを取る
        $p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/u", "/s", "`"$InstalledDll`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Warning "regsvr32 /u が失敗しました（コード: $($p.ExitCode)）" }
        Remove-Item $InstalledDll -Force
    }
    Remove-Item (Join-Path $InstallDir "iroha-server.exe") -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $InstallDir "*.pdb") -Force -ErrorAction SilentlyContinue
    if ((Test-Path $InstallDir) -and -not (Get-ChildItem $InstallDir)) {
        Remove-Item $InstallDir -Force
    }
    Write-Host "==> アンインストール完了"
    return
}

if (-not (Test-Path $BuiltDll)) {
    throw "ビルド済みDLLがありません。先に windows\scripts\build-tip.ps1 を実行してください"
}

New-Item -ItemType Directory -Force $InstallDir | Out-Null
try {
    Copy-Item $BuiltDll $InstalledDll -Force
} catch {
    throw ("DLLのコピーに失敗しました。iroha を使用中のアプリを終了するか、" +
           "MS-IMEに切り替えてから再実行してください: $_")
}
# 変換サーバ（TIPがこのディレクトリから起動する）
if (Test-Path $BuiltServer) {
    Copy-Item $BuiltServer $InstallDir -Force
} else {
    Write-Warning "iroha-server.exe が見つかりません（変換が動きません）: $BuiltServer"
}
# デバッグ用にPDBも並べる（あれば）
foreach ($src in @($BuiltDll, $BuiltServer)) {
    $pdb = [IO.Path]::ChangeExtension($src, ".pdb")
    if (Test-Path $pdb) { Copy-Item $pdb $InstallDir -Force }
}

Write-Host "==> TSF登録: $InstalledDll"
# regsvr32はGUIアプリなのでStart-Process -Waitで終了を待って終了コードを取る
$p = Start-Process "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList "/s", "`"$InstalledDll`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "regsvr32 が失敗しました（コード: $($p.ExitCode)）" }

Write-Host "==> インストール完了"
Write-Host "設定 > 時刻と言語 > 言語と地域 > 日本語 > 言語のオプション > キーボード に「iroha」が現れます。"
Write-Host "現れない場合はサインアウト/サインインしてください。Win+Space で切り替えられます。"
