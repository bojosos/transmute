// DispatchQueue as a Combine scheduler (Apple's Dispatch overlay declares this conformance): SchedulerTimeType (a
// DispatchTime), its Stride (nanoseconds) and SchedulerOptions. Written from Apple's public documentation; the
// implementation is our own, over async, asyncAfter and the timer source.

import Combine

extension DispatchQueue: Scheduler {

    /// A point in time on the dispatch clock.
    public struct SchedulerTimeType: Strideable, Hashable {

        /// The time.
        public var dispatchTime: DispatchTime

        public init(_ time: DispatchTime) { dispatchTime = time }

        public func distance(to other: SchedulerTimeType) -> Stride {
            Stride(.nanoseconds(Int(Int64(bitPattern: other.dispatchTime.rawValue &- dispatchTime.rawValue))))
        }

        public func advanced(by n: Stride) -> SchedulerTimeType {
            SchedulerTimeType(DispatchTime(rawValue: UInt64(bitPattern: Int64(bitPattern: dispatchTime.rawValue) &+ Int64(n.magnitude))))
        }

        public static func == (lhs: SchedulerTimeType, rhs: SchedulerTimeType) -> Bool {
            lhs.dispatchTime.rawValue == rhs.dispatchTime.rawValue
        }

        public func hash(into hasher: inout Hasher) { hasher.combine(dispatchTime.rawValue) }

        /// An interval between two times, in nanoseconds.
        public struct Stride: SchedulerTimeIntervalConvertible, Comparable, SignedNumeric, ExpressibleByFloatLiteral,
                              Hashable, Codable {
            public typealias FloatLiteralType = Double
            public typealias IntegerLiteralType = Int
            public typealias Magnitude = Int

            /// The interval in nanoseconds.
            public var magnitude: Int

            /// The interval as a dispatch time interval.
            public var timeInterval: DispatchTimeInterval { .nanoseconds(magnitude) }

            public init(_ timeInterval: DispatchTimeInterval) { magnitude = timeInterval._nanoseconds }
            public init(floatLiteral value: Double) { magnitude = Int(value * 1_000_000_000) }
            public init(integerLiteral value: Int) { magnitude = value.multipliedReportingOverflow(by: 1_000_000_000).partialValue }
            public init?<T: BinaryInteger>(exactly source: T) {
                guard let value = Int(exactly: source) else { return nil }
                magnitude = value
            }

            public static func seconds(_ s: Int) -> Stride { Stride(.seconds(s)) }
            public static func seconds(_ s: Double) -> Stride { Stride(floatLiteral: s) }
            public static func milliseconds(_ ms: Int) -> Stride { Stride(.milliseconds(ms)) }
            public static func microseconds(_ us: Int) -> Stride { Stride(.microseconds(us)) }
            public static func nanoseconds(_ ns: Int) -> Stride { Stride(.nanoseconds(ns)) }

            public static func < (lhs: Stride, rhs: Stride) -> Bool { lhs.magnitude < rhs.magnitude }
            public static func + (lhs: Stride, rhs: Stride) -> Stride { Stride(.nanoseconds(lhs.magnitude + rhs.magnitude)) }
            public static func - (lhs: Stride, rhs: Stride) -> Stride { Stride(.nanoseconds(lhs.magnitude - rhs.magnitude)) }
            public static func * (lhs: Stride, rhs: Stride) -> Stride { Stride(.nanoseconds(lhs.magnitude * rhs.magnitude)) }
            public static func += (lhs: inout Stride, rhs: Stride) { lhs = lhs + rhs }
            public static func -= (lhs: inout Stride, rhs: Stride) { lhs = lhs - rhs }
            public static func *= (lhs: inout Stride, rhs: Stride) { lhs = lhs * rhs }
        }
    }

    /// Options for scheduling work on a queue.
    public struct SchedulerOptions {
        public var qos: DispatchQoS
        public var flags: DispatchWorkItemFlags
        public var group: DispatchGroup?
        public init(qos: DispatchQoS = .unspecified, flags: DispatchWorkItemFlags = [], group: DispatchGroup? = nil) {
            self.qos = qos
            self.flags = flags
            self.group = group
        }
    }

    public var now: SchedulerTimeType { SchedulerTimeType(DispatchTime.now()) }

    public var minimumTolerance: SchedulerTimeType.Stride { SchedulerTimeType.Stride(.nanoseconds(0)) }

    public func schedule(options: SchedulerOptions?, _ action: @escaping () -> Void) {
        async(group: options?.group, qos: options?.qos ?? .unspecified, flags: options?.flags ?? [], execute: action)
    }

    public func schedule(after date: SchedulerTimeType, tolerance: SchedulerTimeType.Stride, options: SchedulerOptions?,
                         _ action: @escaping () -> Void) {
        asyncAfter(deadline: date.dispatchTime, qos: options?.qos ?? .unspecified, flags: options?.flags ?? [], execute: action)
    }

    public func schedule(after date: SchedulerTimeType, interval: SchedulerTimeType.Stride, tolerance: SchedulerTimeType.Stride,
                         options: SchedulerOptions?, _ action: @escaping () -> Void) -> Cancellable {
        let timer = DispatchSource.makeTimerSource(queue: self)
        timer.setEventHandler(handler: action)
        timer.schedule(deadline: date.dispatchTime, repeating: Double(interval.magnitude) / 1_000_000_000)
        timer.resume()
        return AnyCancellable { timer.cancel() }
    }
}
