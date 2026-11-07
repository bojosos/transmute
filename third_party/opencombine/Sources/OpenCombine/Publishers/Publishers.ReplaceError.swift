//
//  Publishers.ReplaceError.swift
//  OpenCombine
//
//  Created by Bogdan Vlad on 8/29/19.
//

extension Publisher {

    public func replaceError(with output: Output) -> Publishers.ReplaceError<Self> {
        return .init(upstream: self, output: output)
    }
}

extension Publishers {
    public struct ReplaceError<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Never

        public let output: Upstream.Output

        public let upstream: Upstream

        public init(upstream: Upstream,
                    output: Output) {
            self.upstream = upstream
            self.output = output
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Output == Downstream.Input, Downstream.Failure == Failure
        {
            upstream.subscribe(Inner(downstream: subscriber, output: output))
        }
    }
}

extension Publishers.ReplaceError: Equatable
    where Upstream: Equatable, Upstream.Output: Equatable
{}

extension Publishers.ReplaceError {

    private final class Inner<Downstream: Subscriber>
        : Subscriber,
          Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Upstream.Output == Downstream.Input
    {
        typealias Input = Upstream.Output
        typealias Failure = Upstream.Failure

        private let output: Upstream.Output
        private let downstream: Downstream
        private var status = SubscriptionStatus.awaitingSubscription
        private var terminated = false
        private var pendingDemand = Subscribers.Demand.none
        private var lock = UnfairLock.allocate()

        fileprivate init(downstream: Downstream, output: Upstream.Output) {
            self.downstream = downstream
            self.output = output
        }

        deinit {
            lock.deallocate()
        }

        func receive(subscription: Subscription) {
            lock.lock()
            guard case .awaitingSubscription = status else {
                lock.unlock()
                subscription.cancel()
                return
            }
            status = .subscribed(subscription)
            lock.unlock()
            downstream.receive(subscription: self)
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            lock.lock()
            guard case .subscribed = status else {
                lock.unlock()
                return .none
            }
            pendingDemand -= 1
            lock.unlock()
            let demand = downstream.receive(input)
            if demand == .none {
                return .none
            }
            lock.lock()
            pendingDemand += demand
            lock.unlock()
            return demand
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            lock.lock()
            guard case .subscribed = status, !terminated else {
                lock.unlock()
                return
            }
            switch completion {
            case .finished:
                status = .terminal
                lock.unlock()
                downstream.receive(completion: .finished)
            case .failure:
                // If there was no demand from downstream,
                // ReplaceError does not forward the value that
                // replaces the error until it is requested.
                if pendingDemand == .none {
                    terminated = true
                    lock.unlock()
                    return
                }
                status = .terminal
                lock.unlock()
                _ = downstream.receive(output)
                downstream.receive(completion: .finished)
            }
        }

        func request(_ demand: Subscribers.Demand) {
            demand.assertNonZero()
            lock.lock()
            if terminated {
                status = .terminal
                lock.unlock()
                _ = downstream.receive(output)
                downstream.receive(completion: .finished)
                return
            }
            pendingDemand += demand
            guard case let .subscribed(subscription) = status else {
                lock.unlock()
                return
            }
            lock.unlock()
            subscription.request(demand)
        }

        func cancel() {
            lock.lock()
            guard case let .subscribed(subscription) = status else {
                lock.unlock()
                return
            }
            status = .terminal
            lock.unlock()
            subscription.cancel()
        }

        var description: String { return "ReplaceError" }

        var customMirror: Mirror {
            return Mirror(self, children: EmptyCollection())
        }

        var playgroundDescription: Any { return description }
    }
}
