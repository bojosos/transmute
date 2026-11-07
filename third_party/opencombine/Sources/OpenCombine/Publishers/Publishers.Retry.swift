//
//  Publishers.Retry.swift
//  
//
//  Created by Sergej Jaskiewicz on 28.06.2020.
//

extension Publisher {

    public func retry(_ retries: Int) -> Publishers.Retry<Self> {
        return .init(upstream: self, retries: retries)
    }
}

extension Publishers {

    public struct Retry<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let retries: Int?

        public init(upstream: Upstream, retries: Int?) {
            self.upstream = upstream
            self.retries = retries
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Input == Output, Downstream.Failure == Failure
        {
            upstream.subscribe(Inner(parent: self, downstream: subscriber))
        }
    }
}

extension Publishers.Retry: Equatable where Upstream: Equatable {}

extension Publishers.Retry {
    private final class Inner<Downstream: Subscriber>
        : Subscriber,
          Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Downstream.Failure == Failure, Downstream.Input == Output
    {
        typealias Input = Upstream.Output

        typealias Failure = Upstream.Failure

        private enum State {
            case ready(Publishers.Retry<Upstream>, Downstream)
            case terminal
        }

        private enum Chances {
            case finite(Int)
            case infinite
        }

        private let lock = UnfairLock.allocate()

        private var state: State

        private var upstreamSubscription: Subscription?

        private var remaining: Chances

        private var downstreamNeedsSubscription = true

        private var downstreamDemand = Subscribers.Demand.none

        private var completionRecursion = false

        private var needsSubscribe = false

        init(parent: Publishers.Retry<Upstream>, downstream: Downstream) {
            state = .ready(parent, downstream)
            remaining = parent.retries.map(Chances.finite) ?? .infinite
        }

        deinit {
            lock.deallocate()
        }

        func receive(subscription: Subscription) {
            lock.lock()
            guard case let .ready(_, downstream) = state, upstreamSubscription == nil
            else {
                lock.unlock()
                subscription.cancel()
                return
            }
            upstreamSubscription = subscription
            let downstreamDemand = self.downstreamDemand
            let downstreamNeedsSubscription = self.downstreamNeedsSubscription
            self.downstreamNeedsSubscription = false
            lock.unlock()
            if downstreamNeedsSubscription {
                downstream.receive(subscription: self)
            }
            if downstreamDemand != .none {
                subscription.request(downstreamDemand)
            }
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            lock.lock()
            guard case let .ready(_, downstream) = state else {
                lock.unlock()
                return .none
            }
            downstreamDemand -= 1
            lock.unlock()

            let newDemand = downstream.receive(input)

            if newDemand == .none { return .none }

            lock.lock()
            downstreamDemand += newDemand

            if let upstreamSubscription = self.upstreamSubscription {
                lock.unlock()
                upstreamSubscription.request(newDemand)
            } else {
                lock.unlock()
            }

            return .none
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            lock.lock()
            guard case let .ready(parent, downstream) = state else {
                lock.unlock()
                return
            }

            if case .failure = completion {
                upstreamSubscription = nil
                switch remaining {
                case .finite(0):
                    break
                case .finite(let attempts):
                    remaining = .finite(attempts - 1)
                    fallthrough
                case .infinite:
                    if completionRecursion {
                        needsSubscribe = true
                        lock.unlock()
                        return
                    }
                    repeat {
                        completionRecursion = true
                        needsSubscribe = false
                        lock.unlock()
                        parent.upstream.subscribe(self)
                        lock.lock()
                        completionRecursion = false
                    } while needsSubscribe
                    lock.unlock()
                    return
                }
            }

            state = .terminal
            lock.unlock()
            downstream.receive(completion: completion)
        }

        func request(_ demand: Subscribers.Demand) {
            lock.lock()
            guard case .ready = state else {
                lock.unlock()
                return
            }
            downstreamDemand += demand
            if let upstreamSubscription = self.upstreamSubscription {
                lock.unlock()
                upstreamSubscription.request(demand)
            } else {
                lock.unlock()
            }
        }

        func cancel() {
            lock.lock()
            guard case .ready = state else {
                lock.unlock()
                return
            }
            state = .terminal
            if let upstreamSubscription = self.upstreamSubscription {
                lock.unlock()
                upstreamSubscription.cancel()
            } else {
                lock.unlock()
            }
        }

        var description: String { return "Retry" }

        var customMirror: Mirror {
            return Mirror(self, children: EmptyCollection())
        }

        var playgroundDescription: Any { return description }
    }
}
