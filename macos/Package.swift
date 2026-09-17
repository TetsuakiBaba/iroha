// swift-tools-version:6.0
import PackageDescription

// llama.cpp（リポジトリ直下の vendor/dist に scripts/build-llama.sh がスタティックビルドを配置する）
// 相対パスはcwd基準のため、ビルドは必ず macos/ ディレクトリから実行すること
let llamaHeaderFlags: [String] = ["-Xcc", "-I../vendor/dist/include"]
let llamaLinkerSettings: [LinkerSetting] = [
    .unsafeFlags(["-L../vendor/dist/lib"]),
    .linkedLibrary("c++"),
    .linkedFramework("Metal"),
    .linkedFramework("MetalKit"),
    .linkedFramework("Accelerate"),
    .linkedFramework("Foundation"),
]

let package = Package(
    name: "iroha",
    platforms: [.macOS(.v14)],
    dependencies: [
        // 辞書ラティスによるかな漢字変換（azooKey）。候補の読みを辞書で保証するために使う。
        // 辞書データ（Apache-2.0）は vendor/azooKey_dictionary_storage から別途バンドルする
        .package(url: "https://github.com/azooKey/AzooKeyKanaKanjiConverter", .upToNextMinor(from: "0.11.2")),
        // オンデバイス追加学習（LoRA）。iroha-train だけがリンクする（IME本体には入れない）。
        // 0.31.5 以降は swift-tools-version 6.3 を要求し Xcode 26.2 で読めないため固定
        .package(url: "https://github.com/ml-explore/mlx-swift", exact: "0.31.4"),
    ],
    targets: [
        // llama.cpp C APIへのブリッジ
        .systemLibrary(name: "CLlama", path: "Sources/CLlama"),
        // 変換ロジック（ローマ字→かな、zenz変換エンジン）。IMEなしで単体テスト可能
        .target(
            name: "IrohaCore",
            dependencies: [
                "CLlama",
                .product(name: "KanaKanjiConverterModule", package: "AzooKeyKanaKanjiConverter"),
            ],
            swiftSettings: [.unsafeFlags(llamaHeaderFlags)],
            linkerSettings: llamaLinkerSettings
        ),
        // IME本体（InputMethodKit）。scripts/install.shで.appバンドルに組み立てる
        .executableTarget(
            name: "iroha",
            dependencies: ["IrohaCore"],
            swiftSettings: [.swiftLanguageMode(.v5)] + [.unsafeFlags(llamaHeaderFlags)],
            // sqlite3: macOSのユーザ辞書（TextReplacements.db）の読み取りに使う
            linkerSettings: llamaLinkerSettings + [.linkedLibrary("sqlite3")]
        ),
        // 変換エンジンをコマンドラインで試す検証用ハーネス
        .executableTarget(
            name: "iroha-cli",
            dependencies: ["IrohaCore"],
            swiftSettings: [.unsafeFlags(llamaHeaderFlags)],
            linkerSettings: llamaLinkerSettings
        ),
        // 変換記録からの追加学習（MLX で LoRA を学習し GGUF アダプタを書く）。MLX 依存はここに閉じ込め、
        // IME 本体にはリンクしない。学習中は GPU/CPU を占有するので別プロセス iroha-train で動かす
        .target(
            name: "IrohaTrain",
            dependencies: [
                "IrohaCore",
                .product(name: "MLX", package: "mlx-swift"),
                .product(name: "MLXNN", package: "mlx-swift"),
                .product(name: "MLXOptimizers", package: "mlx-swift"),
                .product(name: "MLXRandom", package: "mlx-swift"),
                .product(name: "MLXFast", package: "mlx-swift"),
            ],
            swiftSettings: [.swiftLanguageMode(.v5)] + [.unsafeFlags(llamaHeaderFlags)],
            linkerSettings: llamaLinkerSettings
        ),
        .executableTarget(
            name: "iroha-train",
            dependencies: ["IrohaTrain"],
            swiftSettings: [.swiftLanguageMode(.v5)] + [.unsafeFlags(llamaHeaderFlags)],
            linkerSettings: llamaLinkerSettings
        ),
        .testTarget(
            name: "IrohaTrainTests",
            dependencies: ["IrohaTrain"],
            swiftSettings: [.swiftLanguageMode(.v5)] + [.unsafeFlags(llamaHeaderFlags)],
            linkerSettings: llamaLinkerSettings
        ),
        .testTarget(
            name: "IrohaCoreTests",
            dependencies: ["IrohaCore"],
            swiftSettings: [.unsafeFlags(llamaHeaderFlags)],
            linkerSettings: llamaLinkerSettings
        ),
    ]
)
