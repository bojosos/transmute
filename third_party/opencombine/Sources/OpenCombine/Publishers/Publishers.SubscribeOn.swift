//
//  Publishers.SubscribeOn.swift
//  
//
//  Created by Sergej Jaskiewicz on 02.12.2019.
//

extension Publisher {

    public func subscribe<Context: Scheduler>(
        on scheduler: Context,
        options: Context.SchedulerOptions? = nil
    ) -> Publishers.SubscribeOn<Self, Context> {
        return .init(upstream: self, scheduler: scheduler, options: options)
    }
}

extension Publishers {

    public struct SubscribeOn<Upstream: Publisher, Context: Scheduler>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let scheduler: Context

        public let options: Context.SchedulerOptions?

        public init(upstream: Upstream,
                    scheduler: Context,
                    options: Context.SchedulerOptions?) {
            self.upstream = upstream
            self.scheduler = scheduler
            self.options = options
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure,
                  Upstream.Output == Downstream.Input
        {
            let inner = Inner(scheduler: scheduler,
                              options: options,
                              downstream: subscriber)
            scheduler.schedule(options: options) {
                self.upstream.subscribe(inner)
            }
        }
    }
}

extension Publishers.SubscribeOn {
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

        private let lock = UnfairLock.allocate()
        private let downstream: Downstream
        private let scheduler: Context
        private let options: Context.SchedulerOptions?
        private var state = SubscriptionStatus.awaitingSubscription
        private let upstreamLock = UnfairLock.allocate()

        init(scheduler: Context,
             options: Context.SchedulerOptions?,
             downstream: Downstream) {
            self.downstream = downstream
            self.scheduler = scheduler
            self.options = options
        }

        deinit {
            lock.deallocate()
            upstreamLock.deallocate()
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
            downstream.receive(subscription: self)
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            lock.lock()
            guard case .subscribed = state else {
                lock.unlock()
                return .none
            }
            lock.unlock()
            return downstream.receive(input)
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            lock.lock()
            guard case .subscribed = state else {
                lock.unlock()
                return
            }
            state = .terminal
            lock.unlock()
            downstream.receive(completion: completion)
        }

        func request(_ demand: Subscribers.Demand) {
            lock.lock()
            guard case let .subscribed(subscription) = state else {
                lock.unlock()
                return
            }
            lock.unlock()
            scheduler.schedule(options: options) {
                self.scheduledRequest(demand, subscription: subscription)
            }
        }

        private func scheduledRequest(_ demand: Subscribers.Demand,
                                      subscription: Subscription) {
            upstreamLock.lock()
            subscription.request(demand)
            upstreamLock.unlock()
        }

        func cancel() {
            lock.lock()
            guard case let .subscribed(subscription) = state else {
                lock.unlock()
                return
            }
            state = .terminal
            lock.unlock()
            scheduler.schedule(options: options) {
                self.scheduledCancel(subscription)
            }
        }

        private func scheduledCancel(_ subscription: Subscription) {
            upstreamLock.lock()
            subscription.cancel()
            upstreamLock.unlock()
        }

        var description: String { return "SubscribeOn" }

        var customMirror: Mirror { return Mirror(self, children: EmptyCollection()) }

        var playgroundDescription: Any { return description }
    }
}
