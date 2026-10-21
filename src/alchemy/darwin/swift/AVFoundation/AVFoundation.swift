// Alchemy's project-authored AVFoundation overlay (/usr/lib/swift/
// libswiftAVFoundation.dylib): Apple's typed Swift accessors for the capture
// outputs' pixel-format lists, written from Apple's public documentation.
// The Objective-C properties return NSNumber-wrapped OSTypes; the overlay
// presents them as [OSType]. The classes are reached through the Objective-C
// runtime only (no AVFoundation symbols are bound).

import Foundation
@_exported import AVFoundation // The Clang module (Alchemy's AVFoundation declarations).

extension AVCaptureVideoDataOutput {
    /// The pixel format types the output supports, in order of preference.
    @nonobjc public var availableVideoPixelFormatTypes: [OSType] {
        return _osTypes(__availableVideoCVPixelFormatTypes)
    }
}

extension AVCapturePhotoOutput {
    /// The pixel format types available for uncompressed photo capture.
    @nonobjc public var availablePhotoPixelFormatTypes: [OSType] {
        return _osTypes(__availablePhotoPixelFormatTypes)
    }

    /// The Bayer RAW pixel format types available for RAW photo capture.
    @nonobjc public var availableRawPhotoPixelFormatTypes: [OSType] {
        return _osTypes(__availableRawPhotoPixelFormatTypes)
    }
}

// The OSTypes of an NSArray of NSNumbers, in order.
internal func _osTypes(_ numbers: NSArray) -> [OSType] {
    var types: [OSType] = []
    var iterator = NSFastEnumerationIterator(numbers)
    while let number = iterator.next() {
        types.append((number as! NSNumber).unsignedIntValue)
    }
    return types
}
