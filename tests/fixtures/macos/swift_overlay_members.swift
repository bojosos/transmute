// Project-authored Swift workload for the overlay members OBS's Swift code
// binds: String.cString(using:), NSDictionary's dictionary literals, CGFloat
// and _CFObject from the CoreFoundation overlay, and CMTime's initializer and
// seconds from the CoreMedia overlay.

import CoreFoundation
import CoreMedia
import Foundation

func show(_ bytes: [CChar]?) -> String {
    guard let bytes = bytes else { return "nil" }
    return bytes.map { String($0) }.joined(separator: ",")
}

func cf(_ text: String) -> CFString { return CFStringCreateWithCString(nil, text, 0x0800_0100)!.takeRetainedValue() } // UTF-8

@main
struct SwiftOverlayMembers {
    static func main() {
        print("cString", show("Hi".cString(using: .utf8)), show("caf\u{e9}".cString(using: .ascii)), "caf\u{e9}".cString(using: .utf8)!.count)

        let dictionary: NSDictionary = ["width": 640, "name": "camera"]
        print("dictionary", dictionary.count, CFDictionaryGetCount(unsafeBitCast(dictionary, to: CFDictionary.self)))

        let width: CGFloat = 1.5
        print("cgfloat", width * 2, Double(width + 0.25), CGFloat(3) < width)

        let first = cf("same text"), second = cf("same text")
        print("cfobject", first == second, first.hashValue == second.hashValue, Set([first, second]).count)

        let time = CMTime(value: 3, timescale: 2)
        print("cmtime", time.seconds, time.isValid, CMTime.invalid.seconds.isNaN)
        print("done")
    }
}
