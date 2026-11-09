// Alchemy's project-authored CoreMediaIO overlay (/usr/lib/swift/
// libswiftCoreMediaIO.dylib): the Swift-only members of the camera-extension
// classes, written from Apple's public CoreMediaIO documentation over the
// classes' Objective-C properties. Optional integers are the properties'
// NSNumbers; CMTimes are their CMTime dictionaries (CMTimeCopyAsDictionary).

@_exported import CoreMediaIO // The Clang module (Alchemy's CoreMediaIO declarations).
import CoreMedia
import Foundation

private func dictionary(_ time: CMTime) -> NSDictionary { return CMTimeCopyAsDictionary(time, nil) }

private func number(_ value: Int?) -> NSNumber? { return value.map { $0._bridgeToObjectiveC() } }

private func integer(_ number: NSNumber?) -> Int? { return number.map { Int._unconditionallyBridgeFromObjectiveC($0) } }

extension CMIOExtensionStreamFormat {
    /// A stream format with a format description and the frame durations the stream supports.
    public convenience init(formatDescription: CMFormatDescription, maxFrameDuration: CMTime, minFrameDuration: CMTime,
                            validFrameDurations: [CMTime]?) {
        let durations = validFrameDurations.map { $0.map(dictionary)._bridgeToObjectiveC() }
        self.init(__formatDescription: formatDescription, maxFrameDuration: maxFrameDuration, minFrameDuration: minFrameDuration,
                  validFrameDurations: durations)
    }

    /// The frame durations the stream supports.
    public var validFrameDurations: [CMTime]? {
        guard let durations = __validFrameDurations else { return nil }
        return (0 ..< durations.count).map { CMTimeMakeFromDictionary(durations.object(at: $0) as? NSDictionary) }
    }
}

extension CMIOExtensionStreamProperties {
    /// The index of the active format in the stream's formats.
    public var activeFormatIndex: Int? {
        get { return integer(__activeFormatIndex) }
        set { __activeFormatIndex = number(newValue) }
    }

    /// The frame duration, consistent with the active format's.
    public var frameDuration: CMTime? {
        get { return __frameDuration.map { CMTimeMakeFromDictionary($0) } }
        set { __frameDuration = newValue.map(dictionary) }
    }

    /// The buffer queue size.
    public var sinkBufferQueueSize: Int? {
        get { return integer(__sinkBufferQueueSize) }
        set { __sinkBufferQueueSize = number(newValue) }
    }

    /// The number of buffers the stream requires for startup.
    public var sinkBuffersRequiredForStartup: Int? {
        get { return integer(__sinkBuffersRequiredForStartup) }
        set { __sinkBuffersRequiredForStartup = number(newValue) }
    }
}

extension CMIOExtensionDeviceProperties {
    /// The device's transport type (an IOKit audio transport type constant).
    public var transportType: Int? {
        get { return integer(__transportType) }
        set { __transportType = number(newValue) }
    }
}
