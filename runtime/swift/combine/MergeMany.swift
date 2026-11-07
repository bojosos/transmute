// Publishers.MergeMany and Publisher.merge(with:), which OpenCombine does not
// provide. Written for Alchemy from Combine's published API: the declarations
// follow the documented signatures; the implementation is our own.
//
// Elements from every upstream are delivered to the one downstream in arrival
// order, never more than it demanded; extra elements wait in a queue. The
// publisher finishes once every upstream finished, and fails with the first
// upstream failure (cancelling the others).

extension Publishers {

    public struct MergeMany<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let publishers: [Upstream]

        public init(_ upstream: Upstream...) {
            publishers = upstream
        }

        public init<UpstreamSequence: Swift.Sequence>(_ upstream: UpstreamSequence)
            where Upstream == UpstreamSequence.Element
        {
            publishers = Array(upstream)
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure, Upstream.Output == Downstream.Input
        {
            let merger = Merger(downstream: subscriber, count: publishers.count)
            subscriber.receive(subscription: merger)
            for (index, publisher) in publishers.enumerated() {
                publisher.subscribe(Merger<Downstream>.Side(index: index, merger: merger))
            }
            merger.subscribedAll()
        }

        public func merge(with other: Upstream) -> MergeMany {
            return MergeMany(publishers + [other])
        }
    }
}

extension Publishers.MergeMany: Equatable where Upstream: Equatable {}

extension Publisher {

    public func merge(with other: Self) -> Publishers.MergeMany<Self> {
        return Publishers.MergeMany(self, other)
    }
}

extension Publishers.MergeMany {

    private final class Merger<Downstream: Subscriber>: Subscription, CustomStringConvertible
        where Downstream.Input == Upstream.Output, Downstream.Failure == Upstream.Failure
    {
        struct Side: Subscriber, CustomStringConvertible {
            let index: Int
            let merger: Merger
            let combineIdentifier = CombineIdentifier()

            func receive(subscription: Subscription) { merger.receive(subscription: subscription, from: index) }
            func receive(_ input: Upstream.Output) -> Subscribers.Demand { merger.receive(input) }
            func receive(completion: Subscribers.Completion<Upstream.Failure>) { merger.receive(completion: completion) }
            var description: String { "MergeMany" }
        }

        private let lock = UnfairLock.allocate()
        // Serializes calls into the downstream (values may arrive on several threads).
        private let downstreamLock = UnfairRecursiveLock.allocate()
        private var downstream: Downstream?
        private var subscriptions: [Subscription?]
        private var demand = Subscribers.Demand.none
        private var pending: [Upstream.Output] = []
        private var unfinished: Int
        private var upstreamsFinished = false
        private var requestedBeforeSubscription = Subscribers.Demand.none

        init(downstream: Downstream, count: Int) {
            self.downstream = downstream
            subscriptions = Array(repeating: nil, count: count)
            unfinished = count
        }

        deinit {
            lock.deallocate()
            downstreamLock.deallocate()
        }

        var description: String { "MergeMany" }

        // Every upstream has been asked to subscribe: finishing an empty MergeMany.
        func subscribedAll() {
            lock.lock()
            let finishNow = unfinished == 0 && pending.isEmpty && downstream != nil
            let target = finishNow ? downstream : nil
            if finishNow { downstream = nil }
            lock.unlock()
            if let target = target { send(.finished, to: target) }
        }

        func receive(subscription: Subscription, from index: Int) {
            lock.lock()
            guard downstream != nil, subscriptions[index] == nil else {
                lock.unlock()
                subscription.cancel()
                return
            }
            subscriptions[index] = subscription
            let initial = demand
            lock.unlock()
            if initial > .none { subscription.request(initial) }
        }

        func receive(_ input: Upstream.Output) -> Subscribers.Demand {
            lock.lock()
            guard let target = downstream else { lock.unlock(); return .none }
            guard demand > .none, pending.isEmpty else {
                pending.append(input)
                lock.unlock()
                return .none
            }
            demand -= 1
            lock.unlock()
            let extra = deliver(input, to: target)
            if extra > .none { request(extra) }
            return .none
        }

        func receive(completion: Subscribers.Completion<Upstream.Failure>) {
            lock.lock()
            guard let target = downstream else { lock.unlock(); return }
            switch completion {
            case .failure:
                downstream = nil
                pending.removeAll()
                let others = subscriptions.compactMap { $0 }
                subscriptions = subscriptions.map { _ in nil }
                lock.unlock()
                others.forEach { $0.cancel() }
                send(completion, to: target)
            case .finished:
                unfinished -= 1
                let finishNow = unfinished == 0 && pending.isEmpty
                if finishNow { downstream = nil }
                upstreamsFinished = unfinished == 0
                lock.unlock()
                if finishNow { send(.finished, to: target) }
            }
        }

        func request(_ demand: Subscribers.Demand) {
            lock.lock()
            guard let target = downstream else { lock.unlock(); return }
            self.demand += demand
            // Queued elements first, as far as the demand goes.
            var ready: [Upstream.Output] = []
            while self.demand > .none, !pending.isEmpty {
                ready.append(pending.removeFirst())
                self.demand -= 1
            }
            let finishNow = upstreamsFinished && pending.isEmpty
            if finishNow { downstream = nil }
            let upstream = subscriptions.compactMap { $0 }
            lock.unlock()
            var extra = Subscribers.Demand.none
            for element in ready { extra += deliver(element, to: target) }
            if finishNow { send(.finished, to: target); return }
            if extra > .none {
                lock.lock()
                self.demand += extra
                lock.unlock()
            }
            upstream.forEach { $0.request(demand + extra) }
        }

        func cancel() {
            lock.lock()
            downstream = nil
            pending.removeAll()
            let upstream = subscriptions.compactMap { $0 }
            subscriptions = subscriptions.map { _ in nil }
            lock.unlock()
            upstream.forEach { $0.cancel() }
        }

        private func deliver(_ input: Upstream.Output, to target: Downstream) -> Subscribers.Demand {
            downstreamLock.lock()
            defer { downstreamLock.unlock() }
            return target.receive(input)
        }

        private func send(_ completion: Subscribers.Completion<Upstream.Failure>, to target: Downstream) {
            downstreamLock.lock()
            defer { downstreamLock.unlock() }
            target.receive(completion: completion)
        }
    }
}
