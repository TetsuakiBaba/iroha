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
    targets: [
        // llama.cpp C APIへのブリッジ
        .systemLibrary(name: "CLlama", path: "Sources/CLlama"),
        // 変換ロジック（ローマ字→かな、zenz変換エンジン）。IMEなしで単体テスト可能
        .target(
            name: "IrohaCore",
            dependencies: ["CLlama"],
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
        .testTarget(
            name: "IrohaCoreTests",
            dependencies: ["IrohaCore"],
            swiftSettings: [.unsafeFlags(llamaHeaderFlags)],
            linkerSettings: llamaLinkerSettings
        ),
    ]
)
