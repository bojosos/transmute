//
//  Publishers.Debounce.swift
//  
//
//  Created by Sergej Jaskiewicz on 17.12.2019.
//

extension Publisher {

    public func debounce<Context: Scheduler>(
        for dueTime: Context.SchedulerTimeType.Stride,
        scheduler: Context,
        options: Context.SchedulerOptions? = nil
    ) -> Publishers.Debounce<Self, Context> {
        return .init(upstream: self,
                     dueTime: dueTime,
                     scheduler: scheduler,
                     options: options)
    }
}

extension Publishers {

    public struct Debounce<Upstream: Publisher, Context: Scheduler>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let dueTime: Context.SchedulerTimeType.Stride

        public let scheduler: Context

        public let options: Context.SchedulerOptions?

        public init(upstream: Upstream,
                    dueTime: Context.SchedulerTimeType.Stride,
                    scheduler: Context,
                    options: Context.SchedulerOptions?) {
            self.upstream = upstream
            self.dueTime = dueTime
            self.scheduler = scheduler
            self.options = options
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Failure, Downstream.Input == Output
        {
            let inner = Inner(downstream: subscriber,
                              dueTime: dueTime,
                              scheduler: scheduler,
                              options: options)
            upstream.subscribe(inner)
        }
    }
}

extension Publishers.Debounce {
    private final class Inner<Downstream: Subscriber>
        : Subscriber,
          Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Upstream.Output == Downstream.Input,
              Upstream.Failure == Downstream.Failure
    {
        typealias Input = Upstream.Output

        typealias Failure = Upstream.Failure

        private typealias Generation = UInt64

        private enum CancellerState {
            case pending
            case active(Cancellable)

            fileprivate func cancel() {
                if case let .active(cancellable) = self {
                    cancellable.cancel()
                }
            }
        }

        private let lock = UnfairLock.allocate()

        private let downstreamLock = UnfairRecursiveLock.allocate()

        private let downstream: Downstream

        private let dueTime: Context.SchedulerTimeType.Stride

        private let scheduler: Context

        private let options: Context.SchedulerOptions?

        private var state = SubscriptionStatus.awaitingSubscription

        private var currentCancellers = [Generation : CancellerState]()

        private var currentValue: Output?

        private var currentGeneration: Generation = 0

        private var downstreamDemand = Subscribers.Demand.none

        init(downstream: Downstream,
             dueTime: Context.SchedulerTimeType.Stride,
             scheduler: Context,
             options: Context.SchedulerOptions?) {
            self.downstream = downstream
            self.dueTime = dueTime
            self.scheduler = scheduler
            self.options = options
        }

        deinit {
            lock.deallocate()
            downstreamLock.deallocate()
        }

        func receive(subscription: Subscription) {
            lock.lock()
            guard case .awaitingSubscription = state else {
                lock.unlock()
                subscription.cancel()
                return
            }
            state = .subscribed(subscription)
            lock.unlock()
            downstreamLock.lock()
            downstream.receive(subscription: self)
            downstreamLock.unlock()
            subscription.request(.unlimited)
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            lock.lock()
            guard case .subscribed = state else {
                lock.unlock()
                return .none
            }
            currentGeneration += 1
            let generation = currentGeneration
            currentValue = input
            let due = scheduler.now.advanced(by: dueTime)
            let previousCancellers = self.currentCancellers.take()
            currentCancellers[generation] = .pending
            lock.unlock()
            let newCanceller = scheduler.schedule(after: due,
                                                  interval: dueTime,
                                                  tolerance: scheduler.minimumTolerance,
                                                  options: options) {
                self.due(generation: generation)
            }
            lock.lock()
            currentCancellers[generation] = .active(newCanceller)
            lock.unlock()
            for canceller in previousCancellers.values {
                canceller.cancel()
            }
            return .none
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            lock.lock()
            guard case .subscribed = state else {
                lock.unlock()
                return
            }
            state = .terminal
            let previousCancellers = currentCancellers.take()
            lock.unlock()
            for canceller in previousCancellers.values {
                canceller.cancel()
            }
            scheduler.schedule {
                self.downstreamLock.lock()
                self.downstream.receive(completion: completion)
                self.downstreamLock.unlock()
            }
        }

        func request(_ demand: Subscribers.Demand) {
            lock.lock()
            guard case .subscribed = state else {
                lock.unlock()
                return
            }
            downstreamDemand += demand
            lock.unlock()
        }

        func cancel() {
            lock.lock()
            guard case .subscribed(let subscription) = state else {
                lock.unlock()
                return
            }
            state = .terminal
            let previousCancellers = currentCancellers.take()
            lock.unlock()
            for canceller in previousCancellers.values {
                canceller.cancel()
            }
            subscription.cancel()
        }

        var description: String { return "Debounce" }

        var customMirror: Mirror {
            let children: [Mirror.Child] = [
                ("downstream", downstream),
                ("downstreamDemand", downstreamDemand),
                ("currentValue", currentValue as Any)
            ]
            return Mirror(self, children: children)
        }

        var playgroundDescription: Any { return description }

        private func due(generation: Generation) {
            lock.lock()
            guard case .subscribed = state else {
                lock.unlock()
                return
            }

            // If this condition holds, it means that no values were received
            // in this time frame => we should propagate the current value downstream.
            guard generation == currentGeneration, let value = currentValue else {
                let canceller = currentCancellers[generation]
                lock.unlock()
                canceller?.cancel()
                return
            }

            guard let canceller = currentCancellers[generation].take() else {
                lock.unlock()
                return
            }

            let hasAnyDemand = downstreamDemand != .none
            if hasAnyDemand {
                downstreamDemand -= 1
            }

            lock.unlock()
            canceller.cancel()

            guard hasAnyDemand else { return }

            downstreamLock.lock()
            let newDemand = downstream.receive(value)
            downstreamLock.unlock()

            if newDemand == .none { return }

            lock.lock()
            downstreamDemand += newDemand
            lock.unlock()
        }
    }
}
