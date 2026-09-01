// メニューバー用アイコン(tiff)を生成するワンショットスクリプト。
// Resources/original/menu_*.png（32x32・黒・背景透過）から
// 16px(1x) + 32px(2x) のマルチ解像度TIFFを作る。
// 使い方: swift scripts/gen-icons.swift Resources/
import AppKit

func renderIcon(source sourceURL: URL, to url: URL) {
    guard let source = NSImage(contentsOf: sourceURL) else {
        fatalError("source not found: \(sourceURL.path)")
    }
    var reps: [NSBitmapImageRep] = []
    for pixels in [16, 32] {  // 1x, 2x
        let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
        )!
        NSGraphicsContext.saveGraphicsState()
        let gc = NSGraphicsContext(bitmapImageRep: rep)!
        NSGraphicsContext.current = gc
        gc.imageInterpolation = .high
        source.draw(
            in: CGRect(x: 0, y: 0, width: pixels, height: pixels),
            from: .zero, operation: .sourceOver, fraction: 1.0
        )
        NSGraphicsContext.restoreGraphicsState()
        rep.size = NSSize(width: 16, height: 16)  // 描画後に設定し、32pxを2x(Retina)扱いにする
        reps.append(rep)
    }
    let data = NSBitmapImageRep.representationOfImageReps(in: reps, using: .tiff, properties: [:])!
    try! data.write(to: url)
    print("wrote \(url.path)")
}

let outDir = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
renderIcon(source: outDir.appendingPathComponent("original/menu_い.png"),
           to: outDir.appendingPathComponent("main.tiff"))
renderIcon(source: outDir.appendingPathComponent("original/menu_A.png"),
           to: outDir.appendingPathComponent("en.tiff"))
