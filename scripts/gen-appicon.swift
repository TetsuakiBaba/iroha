// アプリアイコン(AppIcon.icns)を生成するワンショットスクリプト。
// macOS標準の角丸矩形（キャンバスの約80%）に「い」を描く。
// 使い方: swift scripts/gen-appicon.swift Resources/
import AppKit

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
    let ctx = gc.cgContext

    // Big Sur以降のテンプレート: コンテンツはキャンバスの約80%、角丸は辺の約22.4%
    let margin = size * 0.1
    let rect = CGRect(x: margin, y: margin, width: size - margin * 2, height: size - margin * 2)
    let radius = rect.width * 0.224
    let path = NSBezierPath(roundedRect: rect, xRadius: radius, yRadius: radius)

    // 背景: 白→ごく薄いグレーのグラデーション + 細いボーダー
    path.addClip()
    let gradient = NSGradient(
        starting: NSColor(white: 1.0, alpha: 1.0),
        ending: NSColor(white: 0.92, alpha: 1.0)
    )!
    gradient.draw(in: rect, angle: -90)
    NSGraphicsContext.current = gc  // clip解除なしでボーダーを重ねる
    NSColor(white: 0.75, alpha: 1.0).setStroke()
    path.lineWidth = max(1, size * 0.004)
    path.stroke()

    // 「い」をグリフの実描画範囲で中央に配置（gen-icons.swiftと同じ手法）
    let font = NSFont.systemFont(ofSize: rect.width * 0.62, weight: .medium)
    let attrs: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: NSColor(calibratedRed: 0.15, green: 0.15, blue: 0.17, alpha: 1.0),
    ]
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: "い", attributes: attrs))
    let bounds = CTLineGetImageBounds(line, ctx)
    ctx.textPosition = CGPoint(
        x: ((size - bounds.width) / 2 - bounds.minX).rounded(),
        y: ((size - bounds.height) / 2 - bounds.minY).rounded()
    )
    CTLineDraw(line, ctx)
    NSGraphicsContext.restoreGraphicsState()
    return rep
}

let outDir = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
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
