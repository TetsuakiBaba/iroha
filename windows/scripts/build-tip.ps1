# Windows版iroha（iroha-core + テスト + iroha-tip）をビルドし、単体テストを実行する。
# 成果物: windows/build/iroha-tip/iroha-tip.dll (x64)
# -Vulkan でVulkan版llama（vendor/dist-windows-vulkan）とリンクした一式を
# windows/build-vulkan に作る（A/B比較用）。
#
# EAPをContinueにする理由はbuild-llama.ps1のコメントを参照
# （cmake等のstderr警告がリダイレクト環境でErrorRecord化するため）
param([switch]$Vulkan)

$ErrorActionPreference = "Continue"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$SrcDir   = Join-Path $RepoRoot "windows"
$BuildDir = Join-Path $SrcDir ($(if ($Vulkan) { "build-vulkan" } else { "build" }))

# ---- MSVC環境の構築（vswhere優先の自動検出。詳細は msvc-env.ps1） ----
. (Join-Path $PSScriptRoot "msvc-env.ps1")
Initialize-MsvcEnvironment
$vulkanFlags = @()
if ($Vulkan) {
    Initialize-VulkanSdk
    $vulkanFlags = @("-DIROHA_VULKAN=ON")
}

Write-Host "==> CMake configure"
cmake -S $SrcDir -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY `
    @vulkanFlags
if ($LASTEXITCODE -ne 0) { throw "CMake configure に失敗しました" }

Write-Host "==> ビルド"
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました" }

Write-Host "==> 単体テスト"
& (Join-Path $BuildDir "iroha-core\iroha-core-tests.exe")
if ($LASTEXITCODE -ne 0) { throw "単体テストが失敗しました" }

Write-Host "==> 完了: $(Join-Path $BuildDir 'iroha-tip\iroha-tip.dll')"
