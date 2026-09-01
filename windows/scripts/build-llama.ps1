# llama.cppをWindows用にスタティックビルドして vendor/dist-windows に配置する。
# macos/scripts/build-llama.sh のWindows版。
#
# -Backend vulkan でVulkan対応ビルド（vendor/dist-windows-vulkan に分離配置。
# ビルドにはVulkan SDKが必要。実行時はGPUが無ければCPUに自動フォールバック）。
#
# 前提:
#   - Visual Studio（「C++によるデスクトップ開発」ワークロード: MSVC + Windows SDK）
#   - cmake / ninja（PATH上、または pip install --user cmake ninja）
#
# 注意: vendor/dist はmacOS用（Metal依存）なので絶対に上書きしない。
#       ビルドディレクトリも build-windows* に分離する（vendor/llama.cpp/build はmacOS用）。
#       このリポジトリはDropboxでmacOSマシンと同期されているため、分離を崩すとMac環境を壊す。

param([ValidateSet("cpu", "vulkan")][string]$Backend = "cpu")

# 注意: EAPはContinueにする。stderrがリダイレクトされる環境（CI等）では
# ネイティブコマンド（git/cmake）のstderr出力がErrorRecord化し、Stopだと
# 警告や進捗表示だけでスクリプトが即死するため。失敗は$LASTEXITCODEで検出する
$ErrorActionPreference = "Continue"

$LlamaTag = "b10689"

# vendor/ と patches/ はリポジトリルートにある
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$LlamaDir = Join-Path $RepoRoot "vendor\llama.cpp"
$Suffix   = if ($Backend -eq "vulkan") { "-vulkan" } else { "" }
$BuildDir = Join-Path $LlamaDir "build-windows$Suffix"
$DistDir  = Join-Path $RepoRoot "vendor\dist-windows$Suffix"

# ---- Vulkan SDK（-Backend vulkan のとき） ----
$vulkanFlags = @()
if ($Backend -eq "vulkan") {
    . (Join-Path $PSScriptRoot "msvc-env.ps1")
    Initialize-VulkanSdk
    $vulkanFlags = @("-DGGML_VULKAN=ON")
}

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

# ---- MSVC環境の構築（vswhere優先の自動検出。詳細は msvc-env.ps1） ----
. (Join-Path $PSScriptRoot "msvc-env.ps1")
Initialize-MsvcEnvironment

# ---- CMake configure ----
# GGML_OPENMP=OFF: vcomp*.dll への依存を避ける（IMEは配布物のDLL依存を最小にしたい）
# GGML_NATIVE=OFF + AVX2/FMA/F16C/BMI2: MSVCではNATIVEが機能せず無最適化になるため
#   明示する（Haswell 2013年以降のCPUで動くベースライン。無指定だと変換が5倍遅い）
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY: コンパイラ検査でexeリンクを行わない
#   （静的ライブラリしか作らないので不要。不完全なVSインストールでも検査が通る）
Write-Host "==> CMake configure ($Backend)"
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
    @vulkanFlags `
    -DLLAMA_BUILD_TESTS=OFF `
    -DLLAMA_BUILD_EXAMPLES=OFF `
    -DLLAMA_BUILD_SERVER=OFF `
    -DLLAMA_BUILD_TOOLS=OFF `
    -DLLAMA_CURL=OFF
if ($LASTEXITCODE -ne 0) { throw "CMake configure に失敗しました" }

Write-Host "==> ビルド（llamaライブラリのみ）"
cmake --build $BuildDir --target llama
if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました" }

Write-Host "==> $DistDir へ配置"
New-Item -ItemType Directory -Force (Join-Path $DistDir "include") -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Force (Join-Path $DistDir "lib") -ErrorAction Stop | Out-Null
$libs = @(
    "src\llama.lib",
    "ggml\src\ggml.lib",
    "ggml\src\ggml-base.lib",
    "ggml\src\ggml-cpu.lib"
)
if ($Backend -eq "vulkan") {
    $libs += "ggml\src\ggml-vulkan\ggml-vulkan.lib"
}
foreach ($rel in $libs) {
    $src = Join-Path $BuildDir $rel
    if (-not (Test-Path $src)) { throw "成果物が見つかりません: $src" }
    Copy-Item $src (Join-Path $DistDir "lib\") -ErrorAction Stop
}
Copy-Item (Join-Path $LlamaDir "include\llama.h") (Join-Path $DistDir "include\") -ErrorAction Stop
Copy-Item (Join-Path $LlamaDir "ggml\include\*.h") (Join-Path $DistDir "include\") -ErrorAction Stop

Write-Host "==> 完了: $DistDir"
