# llama.cppをWindows用にスタティックビルドして vendor/dist-windows に配置する。
# macos/scripts/build-llama.sh のWindows版。CPUバックエンドのみ（CUDA/Vulkanは今後検討）。
#
# 前提:
#   - Visual Studio 2022（「C++によるデスクトップ開発」ワークロード: MSVC v143 + Windows SDK）
#   - cmake / ninja（PATH上、または pip install --user cmake ninja）
#
# 注意: vendor/dist はmacOS用（Metal依存）なので絶対に上書きしない。
#       ビルドディレクトリも build-windows に分離する（vendor/llama.cpp/build はmacOS用）。
#       このリポジトリはDropboxでmacOSマシンと同期されているため、分離を崩すとMac環境を壊す。

# 注意: EAPはContinueにする。stderrがリダイレクトされる環境（CI等）では
# ネイティブコマンド（git/cmake）のstderr出力がErrorRecord化し、Stopだと
# 警告や進捗表示だけでスクリプトが即死するため。失敗は$LASTEXITCODEで検出する
$ErrorActionPreference = "Continue"

$LlamaTag = "b10689"

# vendor/ と patches/ はリポジトリルートにある
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$LlamaDir = Join-Path $RepoRoot "vendor\llama.cpp"
$BuildDir = Join-Path $LlamaDir "build-windows"
$DistDir  = Join-Path $RepoRoot "vendor\dist-windows"

# ---- llama.cpp の取得（未取得の場合のみ） ----
if (-not (Test-Path $LlamaDir)) {
    Write-Host "==> llama.cpp ($LlamaTag) を取得"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch $LlamaTag `
        https://github.com/ggml-org/llama.cpp $LlamaDir
    if ($LASTEXITCODE -ne 0) { throw "git clone に失敗しました" }
}

# ---- zenzのpre-tokenizer名を認識させるパッチ（適用済みならスキップ） ----
$vocabCpp = Join-Path $LlamaDir "src\llama-vocab.cpp"
if (-not (Select-String -Path $vocabCpp -Pattern "gpt2-small-japanese-char" -Quiet)) {
    Write-Host "==> zenz対応パッチを適用"
    git -C $LlamaDir apply (Join-Path $RepoRoot "patches\llama-cpp-zenz-pretokenizer.patch")
    if ($LASTEXITCODE -ne 0) { throw "パッチの適用に失敗しました" }
}

# ---- MSVCツールセットの検出 ----
# CMakeのVisual Studioジェネレータ（COMベースのVS検出）がこの環境で動かないため、
# INCLUDE/LIB/PATH を直接組み立てて Ninja ジェネレータを使う。
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
    # includeとデスクトップ用libが揃っている（＝完全にインストールされた）最新ツールセットを選ぶ
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
           "「C++によるデスクトップ開発」ワークロード（MSVC v143 + Windows SDK）を追加してください。")
}
Write-Host "==> MSVC: $vcTools"

# ---- Windows SDK の検出 ----
$sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVer = Get-ChildItem (Join-Path $sdkRoot "Include") -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "um\windows.h") } |
    Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
if (-not $sdkVer) { throw "Windows SDK が見つかりません" }
Write-Host "==> Windows SDK: $sdkVer"

# ---- cmake / ninja の検出（PATH → pipユーザースクリプトの順） ----
$pipScripts = "$env:APPDATA\Python\Python310\Scripts"
$toolDirs = @()
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    if (Test-Path (Join-Path $pipScripts "cmake.exe")) { $toolDirs += $pipScripts }
    else { throw "cmake が見つかりません（winget install Kitware.CMake か pip install --user cmake ninja）" }
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    if ((Test-Path (Join-Path $pipScripts "ninja.exe")) -and ($toolDirs -notcontains $pipScripts)) { $toolDirs += $pipScripts }
}

# ---- MSVC環境変数の構築（vcvars64.bat相当） ----
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

# ---- CMake configure ----
# GGML_OPENMP=OFF: vcomp*.dll への依存を避ける（IMEは配布物のDLL依存を最小にしたい）
# GGML_NATIVE=OFF + AVX2/FMA/F16C/BMI2: MSVCではNATIVEが機能せず無最適化になるため
#   明示する（Haswell 2013年以降のCPUで動くベースライン。無指定だと変換が5倍遅い）
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY: コンパイラ検査でexeリンクを行わない
#   （静的ライブラリしか作らないので不要。不完全なVSインストールでも検査が通る）
Write-Host "==> CMake configure"
cmake -S $LlamaDir -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY `
    -DBUILD_SHARED_LIBS=OFF `
    -DGGML_OPENMP=OFF `
    -DGGML_NATIVE=OFF `
    -DGGML_AVX=ON `
    -DGGML_AVX2=ON `
    -DGGML_FMA=ON `
    -DGGML_F16C=ON `
    -DGGML_BMI2=ON `
    -DLLAMA_BUILD_TESTS=OFF `
    -DLLAMA_BUILD_EXAMPLES=OFF `
    -DLLAMA_BUILD_SERVER=OFF `
    -DLLAMA_BUILD_TOOLS=OFF `
    -DLLAMA_CURL=OFF
if ($LASTEXITCODE -ne 0) { throw "CMake configure に失敗しました" }

Write-Host "==> ビルド（llamaライブラリのみ）"
cmake --build $BuildDir --target llama
if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました" }

Write-Host "==> vendor/dist-windows へ配置"
New-Item -ItemType Directory -Force (Join-Path $DistDir "include") -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Force (Join-Path $DistDir "lib") -ErrorAction Stop | Out-Null
$libs = @(
    "src\llama.lib",
    "ggml\src\ggml.lib",
    "ggml\src\ggml-base.lib",
    "ggml\src\ggml-cpu.lib"
)
foreach ($rel in $libs) {
    $src = Join-Path $BuildDir $rel
    if (-not (Test-Path $src)) { throw "成果物が見つかりません: $src" }
    Copy-Item $src (Join-Path $DistDir "lib\") -ErrorAction Stop
}
Copy-Item (Join-Path $LlamaDir "include\llama.h") (Join-Path $DistDir "include\") -ErrorAction Stop
Copy-Item (Join-Path $LlamaDir "ggml\include\*.h") (Join-Path $DistDir "include\") -ErrorAction Stop

Write-Host "==> 完了: $DistDir"
