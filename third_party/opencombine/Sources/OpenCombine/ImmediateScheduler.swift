//
//  ImmediateScheduler.swift
//  
//
//  Created by Sergej Jaskiewicz on 11.06.2019.
//

public struct ImmediateScheduler: Scheduler {

    public struct SchedulerTimeType: Strideable {

        fileprivate init() {}

        public func distance(to other: SchedulerTimeType) -> Stride { return 0 }

        public func advanced(by _: Stride) -> SchedulerTimeType {
            return SchedulerTimeType()
        }

        public struct Stride: ExpressibleByFloatLiteral,
                              Comparable,
                              SignedNumeric,
                              Codable,
                              SchedulerTimeIntervalConvertible {

            public typealias FloatLiteralType = Double

            public typealias IntegerLiteralType = Int

            public typealias Magnitude = Int

            public var magnitude: Int

            public init(_ value: Int) {
                magnitude = value
            }

            public init(integerLiteral value: Int) {
                self.init(value)
            }

            public init(floatLiteral value: Double) {
                self.init(Int(value))
            }

            public init?<BinaryIntegerType: BinaryInteger>(
                exactly source: BinaryIntegerType
            ) {
                guard let magnitude = Int(exactly: source) else {
                    return nil
                }
                self.init(magnitude)
            }

            public static func < (lhs: Stride, rhs: Stride) -> Bool {
                return lhs.magnitude < rhs.magnitude
            }

            public static func * (lhs: Stride, rhs: Stride) -> Stride {
                return Stride(lhs.magnitude * rhs.magnitude)
            }

            public static func + (lhs: Stride, rhs: Stride) -> Stride {
                return Stride(lhs.magnitude + rhs.magnitude)
            }

            public static func - (lhs: Stride, rhs: Stride) -> Stride {
                return Stride(lhs.magnitude - rhs.magnitude)
            }

            public static func -= (lhs: inout Stride, rhs: Stride) {
                lhs.magnitude -= rhs.magnitude
            }

            public static func *= (lhs: inout Stride, rhs: Stride) {
                lhs.magnitude *= rhs.magnitude
            }

            public static func += (lhs: inout Stride, rhs: Stride) {
                lhs.magnitude += rhs.magnitude
            }

            public static func seconds(_: Int) -> Stride { return 0 }

            public static func seconds(_: Double) -> Stride { return 0 }

            public static func milliseconds(_: Int) -> Stride { return 0 }

            public static func microseconds(_: Int) -> Stride { return 0 }

            public static func nanoseconds(_: Int) -> Stride { return 0 }
        }
    }

    public typealias SchedulerOptions = Never

    public static let shared = ImmediateScheduler()

    public func schedule(options: SchedulerOptions?, _ action: @escaping () -> Void) {
        action()
    }

    public var now: SchedulerTimeType { return SchedulerTimeType() }

    public var minimumTolerance: SchedulerTimeType.Stride { return 0 }

    public func schedule(after date: SchedulerTimeType,
                         tolerance: SchedulerTimeType.Stride,
                         options: SchedulerOptions?,
                         _ action: @escaping () -> Void) {
        action()
    }

    public func schedule(after date: SchedulerTimeType,
                         interval: SchedulerTimeType.Stride,
                         tolerance: SchedulerTimeType.Stride,
                         options: SchedulerOptions?,
                         _ action: @escaping () -> Void) -> Cancellable {
        action()
        return Subscriptions.empty
    }
}
