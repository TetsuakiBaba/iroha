// アプリアイコン(AppIcon.icns)を生成するワンショットスクリプト。
// Resources/original/appicon.png（1024x1024・全面デザイン）を
// macOS標準の角丸矩形（キャンバスの約80%・角丸22.4%）に嵌めて各サイズを書き出す。
// 使い方: swift scripts/gen-appicon.swift Resources/
import AppKit

let outDir = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
let sourceURL = outDir.appendingPathComponent("original/appicon.png")
guard let source = NSImage(contentsOf: sourceURL) else {
    fatalError("source not found: \(sourceURL.path)")
}

func renderAppIcon(pixels: Int) -> NSBitmapImageRep {
    let size = CGFloat(pixels)
    let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
    )!
    NSGraphicsContext.saveGraphicsState()
    let gc = NSGraphicsContext(bitmapImageRep: rep)!
    NSGraphicsContext.current = gc
    gc.imageInterpolation = .high

    // Big Sur以降のテンプレート: コンテンツはキャンバスの約80%、角丸は辺の約22.4%
    let margin = size * 0.1
    let rect = CGRect(x: margin, y: margin, width: size - margin * 2, height: size - margin * 2)
    let radius = rect.width * 0.224
    let path = NSBezierPath(roundedRect: rect, xRadius: radius, yRadius: radius)

    NSGraphicsContext.saveGraphicsState()
    path.addClip()
    source.draw(in: rect, from: .zero, operation: .sourceOver, fraction: 1.0)
    NSGraphicsContext.restoreGraphicsState()

    // 背景が白のため、白い場所でも輪郭が見えるよう細いボーダーを重ねる
    NSColor(white: 0.75, alpha: 1.0).setStroke()
    path.lineWidth = max(1, size * 0.004)
    path.stroke()

    NSGraphicsContext.restoreGraphicsState()
    return rep
}

let iconset = FileManager.default.temporaryDirectory
    .appendingPathComponent("iroha-AppIcon-\(UUID().uuidString).iconset")
try! FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

// iconutilが要求する命名で各サイズを書き出す
let entries: [(name: String, pixels: Int)] = [
    ("icon_16x16", 16), ("icon_16x16@2x", 32),
    ("icon_32x32", 32), ("icon_32x32@2x", 64),
    ("icon_128x128", 128), ("icon_128x128@2x", 256),
    ("icon_256x256", 256), ("icon_256x256@2x", 512),
    ("icon_512x512", 512), ("icon_512x512@2x", 1024),
]
for entry in entries {
    let rep = renderAppIcon(pixels: entry.pixels)
    let png = rep.representation(using: .png, properties: [:])!
    try! png.write(to: iconset.appendingPathComponent("\(entry.name).png"))
}

let icns = outDir.appendingPathComponent("AppIcon.icns")
let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", iconset.path, "-o", icns.path]
try! process.run()
process.waitUntilExit()
try? FileManager.default.removeItem(at: iconset)
guard process.terminationStatus == 0 else {
    fatalError("iconutil failed (\(process.terminationStatus))")
}
print("wrote \(icns.path)")
