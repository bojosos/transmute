//
//  Publishers.Throttle.swift
//  
//
//  Created by Stuart Austin on 14/11/2020.
//

extension Publisher {
    // swiftlint:disable generic_type_name line_length

    public func throttle<S>(for interval: S.SchedulerTimeType.Stride,
                            scheduler: S,
                            latest: Bool) -> Publishers.Throttle<Self, S>
    where S: Scheduler
    {
        return .init(upstream: self,
                     interval: interval,
                     scheduler: scheduler,
                     latest: latest)
    }

    // swiftlint:enable generic_type_name line_length
}

extension Publishers {

    public struct Throttle<Upstream, Context>: Publisher
    where Upstream: Publisher, Context: Scheduler
    {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let interval: Context.SchedulerTimeType.Stride

        public let scheduler: Context

        public let latest: Bool

        public init(upstream: Upstream,
                    interval: Context.SchedulerTimeType.Stride,
                    scheduler: Context,
                    latest: Bool) {

            self.upstream = upstream
            self.interval = interval
            self.scheduler = scheduler
            self.latest = latest
        }

        // swiftlint:disable generic_type_name

        public func receive<S>(subscriber: S)
        where S: Subscriber, Upstream.Failure == S.Failure, Upstream.Output == S.Input
        {
            let inner = Inner(interval: interval,
                              scheduler: scheduler,
                              latest: latest,
                              downstream: subscriber)
            upstream.subscribe(inner)
        }

        // swiftlint:enable generic_type_name
    }
}

extension Publishers.Throttle {
    private final class Inner<Downstream: Subscriber>
    : Subscriber,
      Subscription,
      CustomStringConvertible,
      CustomReflectable,
      CustomPlaygroundDisplayConvertible
    where Downstream.Input == Upstream.Output, Downstream.Failure == Upstream.Failure
    {
        typealias Input = Upstream.Output
        typealias Failure = Upstream.Failure

        private enum State {
            case awaitingSubscription(Downstream)
            case subscribed(Subscription, Downstream)
            case pendingTerminal(Subscription, Downstream)
            case terminal
        }

        private let lock = UnfairLock.allocate()
        private let interval: Context.SchedulerTimeType.Stride
        private let scheduler: Context
        private let latest: Bool
        private var state: State
        private let downstreamLock = UnfairRecursiveLock.allocate()

        private var lastEmissionTime: Context.SchedulerTimeType?

        private var pendingInput: Input?
        private var pendingCompletion: Subscribers.Completion<Failure>?

        private var demand: Subscribers.Demand = .none

        private var lastTime: Context.SchedulerTimeType

        init(interval: Context.SchedulerTimeType.Stride,
             scheduler: Context,
             latest: Bool,
             downstream: Downstream) {
            self.state = .awaitingSubscription(downstream)
            self.interval = interval
            self.scheduler = scheduler
            self.latest = latest

            self.lastTime = scheduler.now
        }

        deinit {
            lock.deallocate()
            downstreamLock.deallocate()
        }

        func receive(subscription: Subscription) {
            lock.lock()
            guard case let .awaitingSubscription(downstream) = state else {
                lock.unlock()
                subscription.cancel()
                return
            }
            self.lastTime = scheduler.now

            state = .subscribed(subscription, downstream)
            lock.unlock()

            subscription.request(.unlimited)

            downstreamLock.lock()
            downstream.receive(subscription: self)
            downstreamLock.unlock()
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            lock.lock()
            guard case .subscribed = state else {
                lock.unlock()
                return .none
            }

            let lastTime = scheduler.now
            self.lastTime = lastTime

            guard demand > .none else {
                lock.unlock()
                return .none
            }

            let hasScheduledOutput = (pendingInput != nil || pendingCompletion != nil)

            if hasScheduledOutput && latest {
                pendingInput = input
                lock.unlock()
            } else if !hasScheduledOutput {
                let minimumEmissionTime =
                    lastEmissionTime.map { $0.advanced(by: interval) }

                let emissionTime =
                    minimumEmissionTime.map { Swift.max(lastTime, $0) } ?? lastTime

                demand -= 1

                pendingInput = input
                lock.unlock()

                let action: () -> Void = { [weak self] in
                    self?.scheduledEmission()
                }

                if emissionTime == lastTime {
                    scheduler.schedule(action)
                } else {
                    scheduler.schedule(after: emissionTime, action)
                }
            } else {
                lock.unlock()
            }

            return .none
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            lock.lock()
            guard case let .subscribed(subscription, downstream) = state else {
                lock.unlock()
                return
            }
            let lastTime = scheduler.now
            self.lastTime = lastTime
            state = .pendingTerminal(subscription, downstream)

            let hasScheduledOutput = (pendingInput != nil || pendingCompletion != nil)

            if hasScheduledOutput && pendingCompletion == nil {
                pendingCompletion = completion
                lock.unlock()
            } else if !hasScheduledOutput {
                pendingCompletion = completion
                lock.unlock()

                scheduler.schedule { [weak self] in
                    self?.scheduledEmission()
                }
            } else {
                lock.unlock()
            }
        }

        private func scheduledEmission() {
            lock.lock()

            let downstream: Downstream

            switch state {
            case .awaitingSubscription, .terminal:
                lock.unlock()
                return
            case let .subscribed(_, foundDownstream),
                 let .pendingTerminal(_, foundDownstream):
                downstream = foundDownstream
            }

            if self.pendingInput != nil && self.pendingCompletion == nil {
                lastEmissionTime = scheduler.now
            }

            let pendingInput = self.pendingInput.take()
            let pendingCompletion = self.pendingCompletion.take()

            if pendingCompletion != nil {
                state = .terminal
            }

            lock.unlock()

            downstreamLock.lock()

            let newDemand: Subscribers.Demand
            if let input = pendingInput {
                newDemand = downstream.receive(input)
            } else {
                newDemand = .none
            }

            if let completion = pendingCompletion {
                downstream.receive(completion: completion)
            }
            downstreamLock.unlock()

            guard newDemand > 0 else { return }
            self.lock.lock()
            demand += newDemand
            self.lock.unlock()
        }

        func request(_ demand: Subscribers.Demand) {
            guard demand > 0 else { return }
            lock.lock()
            guard case .subscribed = state else {
                lock.unlock()
                return
            }
            self.demand += demand
            lock.unlock()
        }

        func cancel() {
            lock.lock()

            let subscription: Subscription?
            switch state {
            case let .subscribed(existingSubscription, _),
                 let .pendingTerminal(existingSubscription, _):
                subscription = existingSubscription
            case .awaitingSubscription, .terminal:
                subscription = nil
            }

            state = .terminal
            lock.unlock()

            subscription?.cancel()
        }

        var description: String { return "Throttle" }

        var customMirror: Mirror { return Mirror(self, children: EmptyCollection()) }

        var playgroundDescription: Any { return description }
    }
}
