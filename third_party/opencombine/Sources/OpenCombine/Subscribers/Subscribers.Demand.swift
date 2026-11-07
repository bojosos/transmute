//
//  Subscribers.Demand.swift
//  
//
//  Created by Sergej Jaskiewicz on 10.06.2019.
//

// swiftlint:disable attributes

#if canImport(_Concurrency) && compiler(>=5.5)
import _Concurrency
#endif

extension Subscribers {

    public struct Demand: Equatable,
                          Comparable,
                          Hashable,
                          Codable,
                          CustomStringConvertible
    {
        @usableFromInline
        internal let rawValue: UInt

        @inline(__always)
        internal init(rawValue: UInt) {
            self.rawValue = min(UInt(Int.max) + 1, rawValue)
        }

        @inline(__always)
        public static var unlimited: Demand {
            return Demand(rawValue: .max)
        }

        @inline(__always)
        public static var none: Demand { return .max(0) }

        @inline(__always)
        public static func max(_ value: Int) -> Demand {
            precondition(value >= 0, "demand cannot be negative")
            return Demand(rawValue: UInt(value))
        }

        public var description: String {
            if self == .unlimited {
                return "unlimited"
            } else {
                return "max(\(rawValue))"
            }
        }

        @inline(__always)
        public static func + (lhs: Demand, rhs: Demand) -> Demand {
            switch (lhs, rhs) {
            case (.unlimited, _):
                return .unlimited
            case (_, .unlimited):
                return .unlimited
            default:
                let (sum, isOverflow) = Int(lhs.rawValue)
                    .addingReportingOverflow(Int(rhs.rawValue))
                return isOverflow ? .unlimited : .max(sum)
            }
        }

        @inline(__always)
        public static func += (lhs: inout Demand, rhs: Demand) {
            if lhs == .unlimited { return }
            lhs = lhs + rhs
        }

        @inline(__always)
        public static func + (lhs: Demand, rhs: Int) -> Demand {
            if lhs == .unlimited {
                return .unlimited
            }
            let (sum, isOverflow) = Int(lhs.rawValue).addingReportingOverflow(rhs)
            return isOverflow ? .unlimited : .max(sum)
        }

        @inline(__always)
        public static func += (lhs: inout Demand, rhs: Int) {
            lhs = lhs + rhs
        }

        public static func * (lhs: Demand, rhs: Int) -> Demand {
            if lhs == .unlimited {
                return .unlimited
            }
            let (product, isOverflow) = Int(lhs.rawValue)
                .multipliedReportingOverflow(by: rhs)
            return isOverflow ? .unlimited : .max(product)
        }

        @inline(__always)
        public static func *= (lhs: inout Demand, rhs: Int) {
            lhs = lhs * rhs
        }

        @inline(__always)
        public static func - (lhs: Demand, rhs: Demand) -> Demand {
            switch (lhs, rhs) {
            case (.unlimited, _):
                return .unlimited
            case (_, .unlimited):
                return .none
            default:
                let (difference, isOverflow) = Int(lhs.rawValue)
                    .subtractingReportingOverflow(Int(rhs.rawValue))
                return isOverflow ? .none : .max(difference)
            }
        }

        @inline(__always)
        public static func -= (lhs: inout Demand, rhs: Demand) {
            lhs = lhs - rhs
        }

        @inline(__always)
        public static func - (lhs: Demand, rhs: Int) -> Demand {
            if lhs == .unlimited {
                return .unlimited
            }

            let (difference, isOverflow) = Int(lhs.rawValue)
                .subtractingReportingOverflow(rhs)
            return isOverflow ? .none : .max(difference)
        }

        @inline(__always)
        public static func -= (lhs: inout Demand, rhs: Int) {
            if lhs == .unlimited { return }
            lhs = lhs - rhs
        }

        @inline(__always)
        public static func > (lhs: Demand, rhs: Int) -> Bool {
            if lhs == .unlimited {
                return true
            } else {
                return Int(lhs.rawValue) > rhs
            }
        }

        @inline(__always)
        public static func >= (lhs: Demand, rhs: Int) -> Bool {
            if lhs == .unlimited {
                return true
            } else {
                return Int(lhs.rawValue) >= rhs
            }
        }

        @inline(__always)
        public static func > (lhs: Int, rhs: Demand) -> Bool {
            if rhs == .unlimited {
                return false
            } else {
                return lhs > Int(rhs.rawValue)
            }
        }

        @inline(__always)
        public static func >= (lhs: Int, rhs: Demand) -> Bool {
            if rhs == .unlimited {
                return false
            } else {
                return lhs >= Int(rhs.rawValue)
            }
        }

        @inline(__always)
        public static func < (lhs: Demand, rhs: Int) -> Bool {
            if lhs == .unlimited {
                return false
            } else {
                return Int(lhs.rawValue) < rhs
            }
        }

        @inline(__always)
        public static func < (lhs: Int, rhs: Demand) -> Bool {
            if rhs == .unlimited {
                return true
            } else {
                return lhs < Int(rhs.rawValue)
            }
        }

        @inline(__always)
        public static func <= (lhs: Demand, rhs: Int) -> Bool {
            if lhs == .unlimited {
                return false
            } else {
                return Int(lhs.rawValue) <= rhs
            }
        }

        @inline(__always)
        public static func <= (lhs: Int, rhs: Demand) -> Bool {
            if rhs == .unlimited {
                return true
            } else {
                return lhs <= Int(rhs.rawValue)
            }
        }

        @inline(__always)
        public static func < (lhs: Demand, rhs: Demand) -> Bool {
            switch (lhs, rhs) {
            case (.unlimited, _):
                return false
            case (_, .unlimited):
                return true
            default:
                return lhs.rawValue < rhs.rawValue
            }
        }

        @inline(__always)
        public static func <= (lhs: Demand, rhs: Demand) -> Bool {
            switch (lhs, rhs) {
            case (.unlimited, .unlimited):
                return true
            case (.unlimited, _):
                return false
            case (_, .unlimited):
                return true
            default:
                return lhs.rawValue <= rhs.rawValue
            }
        }

        @inline(__always)
        public static func >= (lhs: Demand, rhs: Demand) -> Bool {
            switch (lhs, rhs) {
            case (.unlimited, .unlimited):
                return true
            case (.unlimited, _):
                return true
            case (_, .unlimited):
                return false
            default:
                return lhs.rawValue >= rhs.rawValue
            }
        }

        @inline(__always)
        public static func > (lhs: Demand, rhs: Demand) -> Bool {
            switch (lhs, rhs) {
            case (.unlimited, .unlimited):
                return false
            case (.unlimited, _):
                return true
            case (_, .unlimited):
                return false
            default:
                return lhs.rawValue > rhs.rawValue
            }
        }

        @inline(__always)
        public static func == (lhs: Demand, rhs: Int) -> Bool {
            if lhs == .unlimited {
                return false
            } else {
                return Int(lhs.rawValue) == rhs
            }
        }

        public static func != (lhs: Demand, rhs: Int) -> Bool {
            if lhs == .unlimited {
                return true
            } else {
                return Int(lhs.rawValue) != rhs
            }
        }

        public static func == (lhs: Int, rhs: Demand) -> Bool {
            if rhs == .unlimited {
                return false
            } else {
                return rhs.rawValue == lhs
            }
        }

        public static func != (lhs: Int, rhs: Demand) -> Bool {
            if rhs == .unlimited {
                return true
            } else {
                return Int(rhs.rawValue) != lhs
            }
        }

        public static func == (lhs: Demand, rhs: Demand) -> Bool {
            return lhs.rawValue == rhs.rawValue
        }

        public var max: Int? {
            if self == .unlimited {
                return nil
            } else {
                return Int(rawValue)
            }
        }

        public init(from decoder: Decoder) throws {
            try self.init(rawValue: decoder.singleValueContainer().decode(UInt.self))
        }

        public func encode(to encoder: Encoder) throws {
            var container = encoder.singleValueContainer()
            try container.encode(rawValue)
        }
    }
}

#if canImport(_Concurrency) && compiler(>=5.5) || compiler(>=5.5.1)
extension Subscribers.Demand: Sendable {}
#endif
