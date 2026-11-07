// ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
// ┃                                                                                     ┃
// ┃                   Auto-generated from GYB template. DO NOT EDIT!                    ┃
// ┃                                                                                     ┃
// ┃                                                                                     ┃
// ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
//
//  RootProtocols.swift
//  OpenCombine
//
//  Created by Sergej Jaskiewicz on 10.06.2019.
//

#if compiler(>=5.7)
public protocol Publisher<Output, Failure> {

    associatedtype Output

    associatedtype Failure: Error

    func receive<Subscriber: Combine.Subscriber>(subscriber: Subscriber)
        where Failure == Subscriber.Failure, Output == Subscriber.Input
}

public protocol Subject<Output, Failure>: AnyObject, Publisher {

    func send(_ value: Output)

    func send(completion: Subscribers.Completion<Failure>)

    func send(subscription: Subscription)
}

public protocol ConnectablePublisher<Output, Failure>: Publisher {

    func connect() -> Cancellable
}

public protocol Subscriber<Input, Failure>: CustomCombineIdentifierConvertible {

    associatedtype Input

    associatedtype Failure: Error

    func receive(subscription: Subscription)

    func receive(_ input: Input) -> Subscribers.Demand

    func receive(completion: Subscribers.Completion<Failure>)
}

public protocol Scheduler<SchedulerTimeType> {

    associatedtype SchedulerTimeType: Strideable
        where SchedulerTimeType.Stride: SchedulerTimeIntervalConvertible

    associatedtype SchedulerOptions

    var now: SchedulerTimeType { get }

    var minimumTolerance: SchedulerTimeType.Stride { get }

    func schedule(options: SchedulerOptions?, _ action: @escaping () -> Void)

    func schedule(after date: SchedulerTimeType,
                  tolerance: SchedulerTimeType.Stride,
                  options: SchedulerOptions?,
                  _ action: @escaping () -> Void)

    func schedule(after date: SchedulerTimeType,
                  interval: SchedulerTimeType.Stride,
                  tolerance: SchedulerTimeType.Stride,
                  options: SchedulerOptions?,
                  _ action: @escaping () -> Void) -> Cancellable
}
#else
public protocol Publisher {

    associatedtype Output

    associatedtype Failure: Error

    func receive<Subscriber: Combine.Subscriber>(subscriber: Subscriber)
        where Failure == Subscriber.Failure, Output == Subscriber.Input
}

public protocol Subject: AnyObject, Publisher {

    func send(_ value: Output)

    func send(completion: Subscribers.Completion<Failure>)

    func send(subscription: Subscription)
}

public protocol ConnectablePublisher: Publisher {

    func connect() -> Cancellable
}

public protocol Subscriber: CustomCombineIdentifierConvertible {

    associatedtype Input

    associatedtype Failure: Error

    func receive(subscription: Subscription)

    func receive(_ input: Input) -> Subscribers.Demand

    func receive(completion: Subscribers.Completion<Failure>)
}

public protocol Scheduler {

    associatedtype SchedulerTimeType: Strideable
        where SchedulerTimeType.Stride: SchedulerTimeIntervalConvertible

    associatedtype SchedulerOptions

    var now: SchedulerTimeType { get }

    var minimumTolerance: SchedulerTimeType.Stride { get }

    func schedule(options: SchedulerOptions?, _ action: @escaping () -> Void)

    func schedule(after date: SchedulerTimeType,
                  tolerance: SchedulerTimeType.Stride,
                  options: SchedulerOptions?,
                  _ action: @escaping () -> Void)

    func schedule(after date: SchedulerTimeType,
                  interval: SchedulerTimeType.Stride,
                  tolerance: SchedulerTimeType.Stride,
                  options: SchedulerOptions?,
                  _ action: @escaping () -> Void) -> Cancellable
}
#endif
