//
//  Publishers.ReplaceEmpty.swift
//  OpenCombine
//
//  Created by Joe Spadafora on 12/10/19.
//

extension Publisher {

    public func replaceEmpty(with output: Output) -> Publishers.ReplaceEmpty<Self> {
        return .init(upstream: self, output: output)
    }
}

extension Publishers {

    public struct ReplaceEmpty<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let output: Upstream.Output

        public let upstream: Upstream

        public init(upstream: Upstream, output: Output) {
            self.upstream = upstream
            self.output = output
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure,
                  Upstream.Output == Downstream.Input
        {
            let inner = Inner(downstream: subscriber, output: output)
            upstream.subscribe(inner)
        }
    }
}

extension Publishers.ReplaceEmpty: Equatable
    where Upstream: Equatable, Upstream.Output: Equatable {}

extension Publishers.ReplaceEmpty {

    private final class Inner<Downstream: Subscriber>
        : Subscriber,
          Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Upstream.Failure == Downstream.Failure,
              Upstream.Output == Downstream.Input
    {

        typealias Input = Upstream.Output
        typealias Failure = Upstream.Failure

        private let output: Output
        private let downstream: Downstream

        private var receivedUpstream = false
        private var lock = UnfairLock.allocate()
        private var downstreamRequested = false
        private var finishedWithoutUpstream = false

        private var status = SubscriptionStatus.awaitingSubscription

        fileprivate init(downstream: Downstream, output: Output) {
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
            subscription.request(.unlimited)
        }

        func receive(_ input: Upstream.Output) -> Subscribers.Demand {
            lock.lock()
            guard case .subscribed = status else {
                lock.unlock()
                return .none
            }
            receivedUpstream = true
            lock.unlock()
            return downstream.receive(input)
        }

        func receive(completion: Subscribers.Completion<Upstream.Failure>) {
            lock.lock()
            guard case .subscribed = status else {
                lock.unlock()
                return
            }
            status = .terminal
            if receivedUpstream {
                lock.unlock()
                downstream.receive(completion: completion)
                return
            }
            switch completion {
            case .finished:
                if downstreamRequested {
                    lock.unlock()
                    _ = downstream.receive(output)
                    downstream.receive(completion: completion)
                    return
                }
                finishedWithoutUpstream = true
                lock.unlock()
            case .failure:
                lock.unlock()
                downstream.receive(completion: completion)
            }
        }

        func request(_ demand: Subscribers.Demand) {
            demand.assertNonZero()
            lock.lock()
            downstreamRequested = true
            if finishedWithoutUpstream {
                lock.unlock()
                _ = downstream.receive(output)
                downstream.receive(completion: .finished)
                return
            }
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

        var description: String { return "ReplaceEmpty" }

        var customMirror: Mirror {
            return Mirror(self, children: EmptyCollection())
        }

        var playgroundDescription: Any { return description }
    }
}
