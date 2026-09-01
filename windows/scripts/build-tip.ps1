# Windows版iroha（iroha-core + テスト + iroha-tip）をビルドし、単体テストを実行する。
# 成果物: windows/build/iroha-tip/iroha-tip.dll (x64)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$SrcDir   = Join-Path $RepoRoot "windows"
$BuildDir = Join-Path $SrcDir "build"

# ---- MSVCツールセットの検出（build-llama.ps1と同方式） ----
$vsRoots = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
)
$vcTools = $null
foreach ($root in $vsRoots) {
    $msvcDir = Join-Path $root "VC\Tools\MSVC"
    if (-not (Test-Path $msvcDir)) { continue }
    $candidates = Get-ChildItem $msvcDir -Directory | Sort-Object Name -Descending
    foreach ($c in $candidates) {
        if ((Test-Path (Join-Path $c.FullName "include\stdbool.h")) -and
            (Test-Path (Join-Path $c.FullName "lib\x64\msvcrt.lib"))) {
            $vcTools = $c.FullName
            break
        }
    }
    if ($vcTools) { break }
}
if (-not $vcTools) {
    throw ("完全なMSVCツールセットが見つかりません。Visual Studio Installer で " +
           "「C++によるデスクトップ開発」ワークロードを追加してください。")
}

$sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVer = Get-ChildItem (Join-Path $sdkRoot "Include") -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "um\windows.h") } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
if (-not $sdkVer) { throw "Windows SDK が見つかりません" }

$pipScripts = "$env:APPDATA\Python\Python310\Scripts"
$toolDirs = @()
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    if (Test-Path (Join-Path $pipScripts "cmake.exe")) { $toolDirs += $pipScripts }
    else { throw "cmake が見つかりません（pip install --user cmake ninja）" }
}

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

Write-Host "==> CMake configure"
cmake -S $SrcDir -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
if ($LASTEXITCODE -ne 0) { throw "CMake configure に失敗しました" }

Write-Host "==> ビルド"
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました" }

Write-Host "==> 単体テスト"
& (Join-Path $BuildDir "iroha-core\iroha-core-tests.exe")
if ($LASTEXITCODE -ne 0) { throw "単体テストが失敗しました" }

Write-Host "==> 完了: $(Join-Path $BuildDir 'iroha-tip\iroha-tip.dll')"
