//
//  Publishers.Scan.swift
//
//  Created by Eric Patey on 26.08.2019.
//

extension Publisher {

    public func scan<Result>(
        _ initialResult: Result,
        _ nextPartialResult: @escaping (Result, Output) -> Result
    ) -> Publishers.Scan<Self, Result> {
        return .init(upstream: self,
                     initialResult: initialResult,
                     nextPartialResult: nextPartialResult)
    }

    public func tryScan<Result>(
        _ initialResult: Result,
        _ nextPartialResult: @escaping (Result, Output) throws -> Result
    ) -> Publishers.TryScan<Self, Result> {
        return .init(upstream: self,
                     initialResult: initialResult,
                     nextPartialResult: nextPartialResult)
    }
}

extension Publishers {

    public struct Scan<Upstream: Publisher, Output>: Publisher {

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let initialResult: Output

        public let nextPartialResult: (Output, Upstream.Output) -> Output

        public init(upstream: Upstream,
                    initialResult: Output,
                    nextPartialResult: @escaping (Output, Upstream.Output) -> Output) {
            self.upstream = upstream
            self.initialResult = initialResult
            self.nextPartialResult = nextPartialResult
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Output == Downstream.Input, Upstream.Failure == Downstream.Failure
        {
            upstream.subscribe(Inner(downstream: subscriber,
                                     initialResult: initialResult,
                                     nextPartialResult: nextPartialResult))
        }
    }

    public struct TryScan<Upstream: Publisher, Output>: Publisher {

        public typealias Failure = Error

        public let upstream: Upstream

        public let initialResult: Output

        public let nextPartialResult: (Output, Upstream.Output) throws -> Output

        public init(
            upstream: Upstream,
            initialResult: Output,
            nextPartialResult: @escaping (Output, Upstream.Output) throws -> Output
        ) {
            self.upstream = upstream
            self.initialResult = initialResult
            self.nextPartialResult = nextPartialResult
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Output == Downstream.Input, Downstream.Failure == Error
        {
            upstream.subscribe(Inner(downstream: subscriber,
                                     initialResult: initialResult,
                                     nextPartialResult: nextPartialResult))
        }
    }
}

extension Publishers.Scan {

    private final class Inner<Downstream: Subscriber>
        : Subscriber,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Upstream.Failure == Downstream.Failure
    {
        // NOTE: this class has been audited for thread safety.
        // Combine doesn't use any locking here.

        typealias Input = Upstream.Output

        typealias Failure = Upstream.Failure

        private let downstream: Downstream

        private let nextPartialResult: (Downstream.Input, Input) -> Downstream.Input

        private var result: Downstream.Input

        fileprivate init(
            downstream: Downstream,
            initialResult: Downstream.Input,
            nextPartialResult: @escaping (Downstream.Input, Input) -> Downstream.Input
        )
        {
            self.downstream = downstream
            self.result = initialResult
            self.nextPartialResult = nextPartialResult
        }

        func receive(subscription: Subscription) {
            downstream.receive(subscription: subscription)
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            result = nextPartialResult(result, input)
            return downstream.receive(result)
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            downstream.receive(completion: completion)
        }

        var description: String { return "Scan" }

        var customMirror: Mirror {
            let children: [Mirror.Child] = [
                ("downstream", downstream),
                ("result", result)
            ]
            return Mirror(self, children: children)
        }

        var playgroundDescription: Any { return description }
    }
}

extension Publishers.TryScan {
    private final class Inner<Downstream: Subscriber>
        : Subscriber,
          Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Downstream.Failure == Error
    {
        // NOTE: this class has been audited for thread safety.

        typealias Input = Upstream.Output

        typealias Failure = Upstream.Failure

        private let downstream: Downstream

        private let nextPartialResult:
            (Downstream.Input, Input) throws -> Downstream.Input

        private var result: Downstream.Input

        private var status = SubscriptionStatus.awaitingSubscription

        private let lock = UnfairLock.allocate()

        private var finished = false

        fileprivate init(
            downstream: Downstream,
            initialResult: Downstream.Input,
            nextPartialResult:
                @escaping (Downstream.Input, Input) throws -> Downstream.Input
        ) {
            self.downstream = downstream
            self.nextPartialResult = nextPartialResult
            self.result = initialResult
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
            do {
                result = try nextPartialResult(result, input)
                return downstream.receive(result)
            } catch {
                lock.lock()
                guard case let .subscribed(subscription) = status else {
                    lock.unlock()
                    return .none
                }
                status = .terminal
                lock.unlock()
                subscription.cancel()
                downstream.receive(completion: .failure(error))
                return .none
            }
        }

        func receive(completion: Subscribers.Completion<Upstream.Failure>) {
            // Combine doesn't use locking in this method!
            guard case .subscribed = status else {
                return
            }
            downstream.receive(completion: completion.eraseError())
        }

        func request(_ demand: Subscribers.Demand) {
            lock.lock()
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

        var description: String { return "TryScan" }

        var customMirror: Mirror {
            lock.lock()
            defer { lock.unlock() }
            let children: [Mirror.Child] = [
                ("downstream", downstream),
                ("status", status),
                ("result", result)
            ]
            return Mirror(self, children: children)
        }

        var playgroundDescription: Any { return description }
    }
}
