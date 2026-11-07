// Recognise text in an iPod screenshot with macOS Vision.
//
// Usage: screentext IMAGE
// Prints a JSON array of {"text", "x", "y", "w", "h", "confidence"} with boxes
// in image pixels, origin top-left. The image is scaled up before recognition
// because the 320x240 display's small fonts read more reliably that way.
import AppKit
import Foundation
import Vision

let scale: CGFloat = 3

guard CommandLine.arguments.count == 2,
      let image = NSImage(contentsOfFile: CommandLine.arguments[1]),
      let source = image.cgImage(forProposedRect: nil, context: nil, hints: nil)
else {
    FileHandle.standardError.write("usage: screentext IMAGE\n".data(using: .utf8)!)
    exit(2)
}

let width = CGFloat(source.width), height = CGFloat(source.height)
guard let context = CGContext(
    data: nil, width: Int(width * scale), height: Int(height * scale),
    bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
else { exit(1) }
context.interpolationQuality = .high
context.draw(source, in: CGRect(x: 0, y: 0, width: width * scale, height: height * scale))
let scaled = context.makeImage()!

let request = VNRecognizeTextRequest()
request.recognitionLevel = .accurate
request.usesLanguageCorrection = false
// English only. Automatic language detection would read the Language screen's
// native-script names, but it makes every launch wait on Neural Engine model
// compilation (over a minute per image, ANECompilerService at full load);
// English-only recognition takes a fraction of a second.
request.recognitionLanguages = ["en-US"]
try VNImageRequestHandler(cgImage: scaled).perform([request])

var boxes: [[String: Any]] = []
for observation in request.results ?? [] {
    guard let candidate = observation.topCandidates(1).first else { continue }
    let box = observation.boundingBox
    boxes.append([
        "text": candidate.string,
        "x": Int((box.minX * width).rounded()),
        "y": Int(((1 - box.maxY) * height).rounded()),
        "w": Int((box.width * width).rounded()),
        "h": Int((box.height * height).rounded()),
        "confidence": Double(candidate.confidence),
    ])
}
let data = try JSONSerialization.data(withJSONObject: boxes, options: [.sortedKeys])
FileHandle.standardOutput.write(data)
FileHandle.standardOutput.write("\n".data(using: .utf8)!)
