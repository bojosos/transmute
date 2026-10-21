// Alchemy's project-authored CoreMedia overlay (/usr/lib/swift/
// libswiftCoreMedia.dylib): Apple's Swift API on CMTime and CMTimeRange,
// written from Apple's public documentation and implemented in Swift from
// CMTime's documented semantics (CMTimeMake, CMTimeMakeWithSeconds,
// CMTimeGetSeconds, CMTimeCompare, the CMTIME_* and CMTIMERANGE_* macros).
// No CoreMedia function is called. CMTime arithmetic (CMTimeAdd and friends,
// with their least-common-timescale and rounding rules) is not provided.

@_exported import CoreMedia // The Clang module (Alchemy's CoreMedia declarations).

extension CMTime {
    /// A valid time of zero (0/1).
    public static var zero: CMTime { return CMTime(value: 0, timescale: 1, flags: .valid, epoch: 0) }
    /// An invalid time (all fields zero).
    public static var invalid: CMTime { return CMTime(value: 0, timescale: 0, flags: [], epoch: 0) }
    /// A valid, indefinite time.
    public static var indefinite: CMTime { return CMTime(value: 0, timescale: 0, flags: [.valid, .indefinite], epoch: 0) }
    /// A valid time of positive infinity.
    public static var positiveInfinity: CMTime { return CMTime(value: 0, timescale: 0, flags: [.valid, .positiveInfinity], epoch: 0) }
    /// A valid time of negative infinity.
    public static var negativeInfinity: CMTime { return CMTime(value: 0, timescale: 0, flags: [.valid, .negativeInfinity], epoch: 0) }

    /// Creates a valid time of `value / timescale` seconds with epoch zero (CMTimeMake).
    public init(value: CMTimeValue, timescale: CMTimeScale) {
        self.init(value: value, timescale: timescale, flags: .valid, epoch: 0)
    }

    /// Creates a time from a number of seconds and a preferred timescale (CMTimeMakeWithSeconds):
    /// invalid for a timescale <= 0 or a NaN; the value is rounded half away from zero; on
    /// overflow the timescale is halved until the value fits or the timescale is 1, and then the
    /// result is positive or negative infinity; hasBeenRounded is set when the result does not
    /// convert back to exactly `seconds`.
    public init(seconds: Double, preferredTimescale: CMTimeScale) {
        guard preferredTimescale > 0, !seconds.isNaN else { self = .invalid; return }
        if seconds.isInfinite { self = seconds > 0 ? .positiveInfinity : .negativeInfinity; return }
        var timescale = preferredTimescale
        while true {
            let scaled = (seconds * Double(timescale)).rounded(.toNearestOrAwayFromZero)
            // Doubles below 2^63 in magnitude convert to Int64 exactly.
            if scaled.magnitude < 9_223_372_036_854_775_808.0 {
                var time = CMTime(value: CMTimeValue(scaled), timescale: timescale)
                if Double(time.value) / Double(timescale) != seconds { time.flags.insert(.hasBeenRounded) }
                self = time
                return
            }
            if timescale == 1 { self = seconds > 0 ? .positiveInfinity : .negativeInfinity; return }
            timescale /= 2
        }
    }

    /// Whether the time is valid.
    public var isValid: Bool { return flags.contains(.valid) }
    /// Whether the time is positive infinity.
    public var isPositiveInfinity: Bool { return isValid && flags.contains(.positiveInfinity) }
    /// Whether the time is negative infinity.
    public var isNegativeInfinity: Bool { return isValid && flags.contains(.negativeInfinity) }
    /// Whether the time is indefinite.
    public var isIndefinite: Bool { return isValid && flags.contains(.indefinite) }
    /// Whether the time is valid and a finite number (not infinite or indefinite).
    public var isNumeric: Bool { return isValid && flags.intersection(.impliedValueFlagsMask).isEmpty }
    /// Whether the time has been rounded.
    public var hasBeenRounded: Bool { return isNumeric && flags.contains(.hasBeenRounded) }

    /// The time in seconds (CMTimeGetSeconds): NaN for invalid or indefinite times, infinity for
    /// the infinite times, otherwise value / timescale.
    public var seconds: Double {
        if !isValid || isIndefinite { return .nan }
        if isPositiveInfinity { return .infinity }
        if isNegativeInfinity { return -.infinity }
        return Double(value) / Double(timescale)
    }
}

// CMTimeCompare: invalid times are equal to each other and greater than any
// other time; positive infinity is next, then indefinite; negative infinity is
// less than any other time; numeric times with different epochs order by
// epoch, otherwise by value / timescale (compared exactly).
internal func _compare(_ time1: CMTime, _ time2: CMTime) -> Int {
    func rank(_ time: CMTime) -> Int {
        if !time.isValid { return 4 }
        if time.isPositiveInfinity { return 3 }
        if time.isIndefinite { return 2 }
        if time.isNegativeInfinity { return 0 }
        return 1
    }
    let rank1 = rank(time1), rank2 = rank(time2)
    if rank1 != rank2 { return rank1 < rank2 ? -1 : 1 }
    if rank1 != 1 { return 0 }
    if time1.epoch != time2.epoch { return time1.epoch < time2.epoch ? -1 : 1 }
    // value1 * timescale2 against value2 * timescale1, in 128 bits.
    let left = time1.value.multipliedFullWidth(by: CMTimeValue(time2.timescale))
    let right = time2.value.multipliedFullWidth(by: CMTimeValue(time1.timescale))
    if left.high != right.high { return left.high < right.high ? -1 : 1 }
    if left.low != right.low { return left.low < right.low ? -1 : 1 }
    return 0
}

extension CMTime: Equatable, Comparable {
    /// Whether two times are equal by CMTimeCompare (1/2 equals 2/4).
    public static func == (time1: CMTime, time2: CMTime) -> Bool { return _compare(time1, time2) == 0 }
    public static func != (time1: CMTime, time2: CMTime) -> Bool { return _compare(time1, time2) != 0 }
    public static func < (time1: CMTime, time2: CMTime) -> Bool { return _compare(time1, time2) < 0 }
    public static func <= (time1: CMTime, time2: CMTime) -> Bool { return _compare(time1, time2) <= 0 }
    public static func > (time1: CMTime, time2: CMTime) -> Bool { return _compare(time1, time2) > 0 }
    public static func >= (time1: CMTime, time2: CMTime) -> Bool { return _compare(time1, time2) >= 0 }
}

public func CMTIME_IS_VALID(_ time: CMTime) -> Bool { return time.isValid }
public func CMTIME_IS_INVALID(_ time: CMTime) -> Bool { return !time.isValid }
public func CMTIME_IS_POSITIVEINFINITY(_ time: CMTime) -> Bool { return time.isPositiveInfinity }
public func CMTIME_IS_NEGATIVEINFINITY(_ time: CMTime) -> Bool { return time.isNegativeInfinity }
public func CMTIME_IS_INDEFINITE(_ time: CMTime) -> Bool { return time.isIndefinite }
public func CMTIME_IS_NUMERIC(_ time: CMTime) -> Bool { return time.isNumeric }
public func CMTIME_HAS_BEEN_ROUNDED(_ time: CMTime) -> Bool { return time.hasBeenRounded }

extension CMTimeRange {
    /// The empty range starting at zero.
    public static var zero: CMTimeRange { return CMTimeRange(start: .zero, duration: .zero) }
    /// An invalid range.
    public static var invalid: CMTimeRange { return CMTimeRange(start: .invalid, duration: .invalid) }

    /// Whether the range is valid: a valid start and a valid, non-negative duration with epoch zero.
    public var isValid: Bool {
        return start.isValid && duration.isValid && duration.epoch == 0 && duration.value >= 0
    }
    /// Whether the range is valid and its start or duration is indefinite.
    public var isIndefinite: Bool { return isValid && (start.isIndefinite || duration.isIndefinite) }
    /// Whether the range is valid and its duration is zero.
    public var isEmpty: Bool { return isValid && duration == .zero }
}

extension CMTimeRange: Equatable {
    /// Whether the ranges have equal starts and durations (CMTimeRangeEqual).
    public static func == (range1: CMTimeRange, range2: CMTimeRange) -> Bool {
        return range1.start == range2.start && range1.duration == range2.duration
    }
    public static func != (range1: CMTimeRange, range2: CMTimeRange) -> Bool { return !(range1 == range2) }
}

public func CMTIMERANGE_IS_VALID(_ range: CMTimeRange) -> Bool { return range.isValid }
public func CMTIMERANGE_IS_INVALID(_ range: CMTimeRange) -> Bool { return !range.isValid }
public func CMTIMERANGE_IS_INDEFINITE(_ range: CMTimeRange) -> Bool { return range.isIndefinite }
public func CMTIMERANGE_IS_EMPTY(_ range: CMTimeRange) -> Bool { return range.isEmpty }
