// Controlled clipboard fixture for live Deskflow image transfer tests.
// read-bmp writes only the current BMP bytes to stdout; capture them, do not log them.
import AppKit
let pb = NSPasteboard.general
let mode = CommandLine.arguments[1]
if mode == "read-bmp" {
    guard let data = pb.data(forType: NSPasteboard.PasteboardType("com.microsoft.bmp")) else { exit(5) }
    FileHandle.standardOutput.write(data)
} else if mode == "write" {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 4, pixelsHigh: 4,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 16, bitsPerPixel: 32)!
    for i in 0..<16 {
        bitmap.bitmapData![i * 4] = 0
        bitmap.bitmapData![i * 4 + 1] = 0
        bitmap.bitmapData![i * 4 + 2] = 255
        bitmap.bitmapData![i * 4 + 3] = 255
    }
    let img = NSImage(cgImage: bitmap.cgImage!, size: NSSize(width: 4, height: 4))
    pb.clearContents()
    guard pb.writeObjects([img]) else { exit(1) }
    print("image written")
} else {
    guard let image = NSImage(pasteboard: pb), let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else { exit(2) }
    let bitmap = NSBitmapImageRep(cgImage: cg)
    guard bitmap.pixelsWide == 4 && bitmap.pixelsHigh == 4 else { exit(3) }
    for y in 0..<4 { for x in 0..<4 {
        guard let c = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.sRGB),
              c.redComponent > 0.99 && c.greenComponent < 0.01 && c.blueComponent < 0.01 else { exit(4) }
    } }
    print("PASS Linux to Mac image: 4x4 red pixels")
}
