# MSVCツールチェーンの検出と環境変数（PATH/INCLUDE/LIB）の構築。
# build-llama.ps1 / build-tip.ps1 からドットソースで読み込む。
#
# CMakeのVisual Studioジェネレータ（COMベースのVS検出）が動かない環境が
# あるため、vcvarsに頼らず自前で組み立てる。検出は
#   1) vswhere.exe（あれば。エディション・バージョン非依存）
#   2) 既知パスのワイルドカード探索（C:\Program Files\Microsoft Visual Studio\*\*）
# の順に試し、ヘッダとデスクトップ用CRTライブラリが揃った最新ツールセットを選ぶ。

function Initialize-MsvcEnvironment {
    # ---- Visual Studioのインストール先候補 ----
    $vsRoots = @()
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -products * -latest `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($LASTEXITCODE -eq 0 -and $found) { $vsRoots += $found }
    }
    foreach ($base in @("$env:ProgramFiles\Microsoft Visual Studio",
                        "${env:ProgramFiles(x86)}\Microsoft Visual Studio")) {
        if (Test-Path $base) {
            $vsRoots += Get-ChildItem "$base\*\*" -Directory -ErrorAction SilentlyContinue |
                Sort-Object FullName -Descending | Select-Object -ExpandProperty FullName
        }
    }

    # ---- includeとデスクトップ用libが揃った最新ツールセットを選ぶ ----
    $vcTools = $null
    foreach ($root in $vsRoots) {
        $msvcDir = Join-Path $root "VC\Tools\MSVC"
        if (-not (Test-Path $msvcDir)) { continue }
        $candidates = Get-ChildItem $msvcDir -Directory | Sort-Object Name -Descending
        foreach ($candidate in $candidates) {
            if ((Test-Path (Join-Path $candidate.FullName "include\stdbool.h")) -and
                (Test-Path (Join-Path $candidate.FullName "lib\x64\msvcrt.lib"))) {
                $vcTools = $candidate.FullName
                break
            }
        }
        if ($vcTools) { break }
    }
    if (-not $vcTools) {
        throw ("完全なMSVCツールセットが見つかりません。Visual Studio Installer で " +
               "「C++によるデスクトップ開発」ワークロード（MSVC + Windows SDK）を追加してください。")
    }
    Write-Host "==> MSVC: $vcTools"

    # ---- Windows SDK ----
    $sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
    $sdkVer = Get-ChildItem (Join-Path $sdkRoot "Include") -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName "um\windows.h") } |
        Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
    if (-not $sdkVer) { throw "Windows SDK が見つかりません" }
    Write-Host "==> Windows SDK: $sdkVer"

    # ---- cmake / ninja（PATH → pipユーザースクリプトの順） ----
    $toolDirs = @()
    $pipScripts = "$env:APPDATA\Python\Python310\Scripts"
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        if (Test-Path (Join-Path $pipScripts "cmake.exe")) { $toolDirs += $pipScripts }
        else { throw "cmake が見つかりません（pip install --user cmake ninja）" }
    }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        if ((Test-Path (Join-Path $pipScripts "ninja.exe")) -and ($toolDirs -notcontains $pipScripts)) {
            $toolDirs += $pipScripts
        }
    }

    # ---- 環境変数の構築（vcvars64.bat相当） ----
    $env:PATH = (@(
        (Join-Path $vcTools "bin\Hostx64\x64"),
        (Join-Path $sdkRoot "bin\$sdkVer\x64")
    ) + $toolDirs + @($env:PATH)) -join ";"
    $env:INCLUDE = @(
        (Join-Path $vcTools "include"),
        (Join-Path $sdkRoot "Include\$sdkVer\ucrt"),
        (Join-Path $sdkRoot "Include\$sdkVer\um"),
        (Join-Path $sdkRoot "Include\$sdkVer\shared"),
        (Join-Path $sdkRoot "Include\$sdkVer\winrt")
    ) -join ";"
    $env:LIB = @(
        (Join-Path $vcTools "lib\x64"),
        (Join-Path $sdkRoot "Lib\$sdkVer\ucrt\x64"),
        (Join-Path $sdkRoot "Lib\$sdkVer\um\x64")
    ) -join ";"
}
