//
//  Publishers.CollectByCount.swift
//  
//
//  Created by Sergej Jaskiewicz on 24.12.2019.
//

extension Publisher {

    public func collect(_ count: Int) -> Publishers.CollectByCount<Self> {
        return .init(upstream: self, count: count)
    }
}

extension Publishers {

    public struct CollectByCount<Upstream: Publisher>: Publisher {

        public typealias Output = [Upstream.Output]

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let count: Int

        public init(upstream: Upstream, count: Int) {
            self.upstream = upstream
            self.count = count
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Failure, Downstream.Input == Output
        {
            upstream.subscribe(Inner(downstream: subscriber, count: count))
        }
    }
}

extension Publishers.CollectByCount: Equatable where Upstream: Equatable {}

extension Publishers.CollectByCount {
    private final class Inner<Downstream: Subscriber>
        : Subscriber,
          Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Downstream.Input == [Upstream.Output],
              Downstream.Failure == Upstream.Failure
    {
        typealias Input = Upstream.Output

        typealias Failure = Upstream.Failure

        private let downstream: Downstream

        private let count: Int

        private var buffer: [Input] = []

        private var subscription: Subscription?

        private var finished = false

        private let lock = UnfairLock.allocate()

        init(downstream: Downstream, count: Int) {
            self.downstream = downstream
            self.count = count
        }

        deinit {
            lock.deallocate()
        }

        func receive(subscription: Subscription) {
            lock.lock()
            if finished || self.subscription != nil {
                lock.unlock()
                subscription.cancel()
                return
            }
            self.subscription = subscription
            lock.unlock()
            downstream.receive(subscription: self)
        }

        func receive(_ input: Upstream.Output) -> Subscribers.Demand {
            lock.lock()
            if subscription == nil {
                lock.unlock()
                return .none
            }
            buffer.append(input)
            guard buffer.count == count else {
                lock.unlock()
                return .none
            }
            let output = self.buffer.take()
            lock.unlock()
            return downstream.receive(output) * count
        }

        func receive(completion: Subscribers.Completion<Upstream.Failure>) {
            lock.lock()
            subscription = nil
            finished = true
            switch completion {
            case .finished:
                if buffer.isEmpty {
                    lock.unlock()
                } else {
                    let buffer = self.buffer.take()
                    lock.unlock()
                    _ = downstream.receive(buffer)
                }
            case .failure:
                buffer = []
                lock.unlock()
            }
            downstream.receive(completion: completion)
        }

        func request(_ demand: Subscribers.Demand) {
            demand.assertNonZero()
            lock.lock()
            if let subscription = self.subscription {
                lock.unlock()
                subscription.request(demand * count)
            } else {
                lock.unlock()
            }
        }

        func cancel() {
            lock.lock()
            if let subscription = self.subscription.take() {
                buffer = []
                finished = true
                lock.unlock()
                subscription.cancel()
            } else {
                lock.unlock()
            }
        }

        var description: String { return "CollectByCount" }

        var customMirror: Mirror {
            lock.lock()
            defer { lock.unlock() }
            let children: [Mirror.Child] = [
                ("downstream", downstream),
                ("upstreamSubscription", subscription as Any),
                ("buffer", buffer),
                ("count", count)
            ]
            return Mirror(self, children: children)
        }

        var playgroundDescription: Any { return description }
    }
}
