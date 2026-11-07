//
//  Scheduler.swift
//  
//
//  Created by Sergej Jaskiewicz on 11.06.2019.
//

public protocol SchedulerTimeIntervalConvertible {

    static func seconds(_ s: Int) -> Self

    static func seconds(_ s: Double) -> Self

    static func milliseconds(_ ms: Int) -> Self

    static func microseconds(_ us: Int) -> Self

    static func nanoseconds(_ ns: Int) -> Self
}

extension Scheduler {

    public func schedule(after date: SchedulerTimeType,
                         _ action: @escaping () -> Void) {
        schedule(after: date, tolerance: minimumTolerance, action)
    }

    public func schedule(_ action: @escaping () -> Void) {
        schedule(options: nil, action)
    }

    public func schedule(after date: SchedulerTimeType,
                         tolerance: SchedulerTimeType.Stride,
                         _ action: @escaping () -> Void) {
        schedule(after: date, tolerance: tolerance, options: nil, action)
    }

    public func schedule(after date: SchedulerTimeType,
                         interval: SchedulerTimeType.Stride,
                         tolerance: SchedulerTimeType.Stride,
                         _ action: @escaping () -> Void) -> Cancellable {
        return schedule(after: date,
                        interval: interval,
                        tolerance: tolerance,
                        options: nil,
                        action)
    }

    public func schedule(after date: SchedulerTimeType,
                         interval: SchedulerTimeType.Stride,
                         _ action: @escaping () -> Void) -> Cancellable {
        return schedule(after: date,
                        interval: interval,
                        tolerance: minimumTolerance,
                        action)
    }
}
