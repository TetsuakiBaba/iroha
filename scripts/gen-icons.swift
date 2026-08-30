// メニューバー用アイコン(tiff)を生成するワンショットスクリプト
// 使い方: swift scripts/gen-icons.swift Resources/
import AppKit

func renderIcon(text: String, to url: URL) {
    let sizes: [CGFloat] = [16, 32]  // 1x, 2x
    var reps: [NSBitmapImageRep] = []
    for size in sizes {
        let rep = NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size),
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0
        )!
        NSGraphicsContext.saveGraphicsState()
        let gc = NSGraphicsContext(bitmapImageRep: rep)!
        NSGraphicsContext.current = gc
        let ctx = gc.cgContext
        let font = NSFont.systemFont(ofSize: size * 0.75, weight: .medium)
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor.black]
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attrs))
        // 行高ではなくグリフの実描画範囲(image bounds)で中央に置く。
        // 行高基準だとディセンダ/レディング分だけ縦にズレて見える。
        let bounds = CTLineGetImageBounds(line, ctx)
        ctx.textPosition = CGPoint(
            x: ((size - bounds.width) / 2 - bounds.minX).rounded(),
            y: ((size - bounds.height) / 2 - bounds.minY).rounded()
        )
        CTLineDraw(line, ctx)
        NSGraphicsContext.restoreGraphicsState()
        rep.size = NSSize(width: 16, height: 16)  // 描画後に設定し、32pxを2x(Retina)扱いにする
        reps.append(rep)
    }
    let data = NSBitmapImageRep.representationOfImageReps(in: reps, using: .tiff, properties: [:])!
    try! data.write(to: url)
    print("wrote \(url.path)")
}

let outDir = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
renderIcon(text: "い", to: outDir.appendingPathComponent("main.tiff"))
renderIcon(text: "A", to: outDir.appendingPathComponent("en.tiff"))
