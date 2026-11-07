// ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
// ┃                                                                                     ┃
// ┃                   Auto-generated from GYB template. DO NOT EDIT!                    ┃
// ┃                                                                                     ┃
// ┃                                                                                     ┃
// ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
//
//  Publishers.Catch.swift
//  
//
//  Created by Sergej Jaskiewicz on 25.12.2019.
//

extension Publisher {

    public func `catch`<NewPublisher: Publisher>(
        _ handler: @escaping (Failure) -> NewPublisher
    ) -> Publishers.Catch<Self, NewPublisher>
        where NewPublisher.Output == Output
    {
        return .init(upstream: self, handler: handler)
    }

    public func tryCatch<NewPublisher: Publisher>(
        _ handler: @escaping (Failure) throws -> NewPublisher
    ) -> Publishers.TryCatch<Self, NewPublisher>
        where NewPublisher.Output == Output
    {
        return .init(upstream: self, handler: handler)
    }
}

extension Publishers {

    public struct Catch<Upstream: Publisher, NewPublisher: Publisher>: Publisher
        where Upstream.Output == NewPublisher.Output
    {
        public typealias Output = Upstream.Output

        public typealias Failure = NewPublisher.Failure

        public let upstream: Upstream

        public let handler: (Upstream.Failure) -> NewPublisher

        public init(upstream: Upstream,
                    handler: @escaping (Upstream.Failure) -> NewPublisher) {
            self.upstream = upstream
            self.handler = handler
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Input == Output, Downstream.Failure == Failure
        {
            let inner = Inner(downstream: subscriber, handler: handler)
            let uncaughtS = Inner.UncaughtS(inner: inner)
            upstream.subscribe(uncaughtS)
        }
    }

    public struct TryCatch<Upstream: Publisher, NewPublisher: Publisher>: Publisher
        where Upstream.Output == NewPublisher.Output
    {
        public typealias Output = Upstream.Output

        public typealias Failure = Error

        public let upstream: Upstream

        public let handler: (Upstream.Failure) throws -> NewPublisher

        public init(upstream: Upstream,
                    handler: @escaping (Upstream.Failure) throws -> NewPublisher) {
            self.upstream = upstream
            self.handler = handler
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Input == Output, Downstream.Failure == Failure
        {
            let inner = Inner(downstream: subscriber, handler: handler)
            let uncaughtS = Inner.UncaughtS(inner: inner)
            upstream.subscribe(uncaughtS)
        }
    }
}

extension Publishers.Catch {
    private final class Inner<Downstream: Subscriber>
        : Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Downstream.Input == Upstream.Output,
              Downstream.Failure == NewPublisher.Failure
    {
        struct UncaughtS: Subscriber,
                          CustomStringConvertible,
                          CustomReflectable,
                          CustomPlaygroundDisplayConvertible
        {
            typealias Input = Upstream.Output

            typealias Failure = Upstream.Failure

            let inner: Inner

            var combineIdentifier: CombineIdentifier { return inner.combineIdentifier }

            func receive(subscription: Subscription) {
                inner.receivePre(subscription: subscription)
            }

            func receive(_ input: Input) -> Subscribers.Demand {
                return inner.receivePre(input)
            }

            func receive(completion: Subscribers.Completion<Failure>) {
                return inner.receivePre(completion: completion)
            }

            var description: String { return inner.description }

            var customMirror: Mirror { return inner.customMirror }

            var playgroundDescription: Any { return description }
        }

        struct CaughtS: Subscriber,
                        CustomStringConvertible,
                        CustomReflectable,
                        CustomPlaygroundDisplayConvertible
        {
            typealias Input = NewPublisher.Output

            typealias Failure = NewPublisher.Failure

            let inner: Inner

            var combineIdentifier: CombineIdentifier { return inner.combineIdentifier }

            func receive(subscription: Subscription) {
                inner.receivePost(subscription: subscription)
            }

            func receive(_ input: Input) -> Subscribers.Demand {
                return inner.receivePost(input)
            }

            func receive(completion: Subscribers.Completion<Failure>) {
                inner.receivePost(completion: completion)
            }

            var description: String { return inner.description }

            var customMirror: Mirror { return inner.customMirror }

            var playgroundDescription: Any { return description }
        }

        private enum State {
            case pendingPre
            case pre(Subscription)
            case pendingPost
            case post(Subscription)
            case cancelled
        }

        private let lock = UnfairLock.allocate()

        private var demand = Subscribers.Demand.none

        private var state = State.pendingPre

        private let downstream: Downstream

        private let handler: (Upstream.Failure) -> NewPublisher

        init(downstream: Downstream,
             handler: @escaping (Upstream.Failure) -> NewPublisher) {
            self.downstream = downstream
            self.handler = handler
        }

        deinit {
            lock.deallocate()
        }

        func receivePre(subscription: Subscription) {
            lock.lock()
            guard case .pendingPre = state else {
                lock.unlock()
                subscription.cancel()
                return
            }
            state = .pre(subscription)
            lock.unlock()
            downstream.receive(subscription: self)
        }

        func receivePre(_ input: Upstream.Output) -> Subscribers.Demand {
            lock.lock()
            demand -= 1
            lock.unlock()
            let newDemand = downstream.receive(input)
            lock.lock()
            demand += newDemand
            lock.unlock()
            return newDemand
        }

        func receivePre(completion: Subscribers.Completion<Upstream.Failure>) {
            switch completion {
            case .finished:
                lock.lock()
                switch state {
                case .pre:
                    state = .cancelled
                    lock.unlock()
                    downstream.receive(completion: .finished)
                case .pendingPre, .pendingPost, .post, .cancelled:
                    lock.unlock()
                }
            case .failure(let error):
                lock.lock()
                switch state {
                case .pre:
                    state = .pendingPost
                    lock.unlock()
                    handler(error).subscribe(CaughtS(inner: self))
                case .cancelled:
                    lock.unlock()
                case .pendingPre, .post, .pendingPost:
                    completionBeforeSubscription()
                }
            }
        }

        func receivePost(subscription: Subscription) {
            lock.lock()
            guard case .pendingPost = state else {
                lock.unlock()
                subscription.cancel()
                return
            }
            state = .post(subscription)
            let demand = self.demand
            lock.unlock()
            if demand > 0 {
                subscription.request(demand)
            }
        }

        func receivePost(_ input: NewPublisher.Output) -> Subscribers.Demand {
            return downstream.receive(input)
        }

        func receivePost(completion: Subscribers.Completion<NewPublisher.Failure>) {
            lock.lock()
            guard case .post = state else {
                lock.unlock()
                return
            }
            state = .cancelled
            lock.unlock()
            downstream.receive(completion: completion)
        }

        func request(_ demand: Subscribers.Demand) {
            demand.assertNonZero()
            lock.lock()
            switch state {
            case .pendingPre:
                // The client is only able to call the `request` method after we've sent
                // `self` downstream. We only do it in the `receivePre(subscription:)`
                // method, after setting `state` to `pre`.
                // After that `state` never becomes `pendingPre`.
                requestBeforeSubscription()
            case let .pre(subscription):
                self.demand += demand
                lock.unlock()
                subscription.request(demand)
            case .pendingPost:
                self.demand += demand
                lock.unlock()
            case let .post(subscription):
                lock.unlock()
                subscription.request(demand)
            case .cancelled:
                lock.unlock()
            }
        }

        func cancel() {
            lock.lock()
            switch state {
            case let .pre(subscription), let .post(subscription):
                state = .cancelled
                lock.unlock()
                subscription.cancel()
            case .pendingPre, .pendingPost, .cancelled:
                lock.unlock()
            }
        }

        var description: String { return "Catch" }

        var customMirror: Mirror {
            let children: [Mirror.Child] = [
                ("downstream", downstream),
                ("demand", demand)
            ]
            return Mirror(self, children: children)
        }

        var playgroundDescription: Any { return description }
    }
}

extension Publishers.TryCatch {
    private final class Inner<Downstream: Subscriber>
        : Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Downstream.Input == Upstream.Output,
              Downstream.Failure == Error
    {
        struct UncaughtS: Subscriber,
                          CustomStringConvertible,
                          CustomReflectable,
                          CustomPlaygroundDisplayConvertible
        {
            typealias Input = Upstream.Output

            typealias Failure = Upstream.Failure

            let inner: Inner

            var combineIdentifier: CombineIdentifier { return inner.combineIdentifier }

            func receive(subscription: Subscription) {
                inner.receivePre(subscription: subscription)
            }

            func receive(_ input: Input) -> Subscribers.Demand {
                return inner.receivePre(input)
            }

            func receive(completion: Subscribers.Completion<Failure>) {
                return inner.receivePre(completion: completion)
            }

            var description: String { return inner.description }

            var customMirror: Mirror { return inner.customMirror }

            var playgroundDescription: Any { return description }
        }

        struct CaughtS: Subscriber,
                        CustomStringConvertible,
                        CustomReflectable,
                        CustomPlaygroundDisplayConvertible
        {
            typealias Input = NewPublisher.Output

            typealias Failure = NewPublisher.Failure

            let inner: Inner

            var combineIdentifier: CombineIdentifier { return inner.combineIdentifier }

            func receive(subscription: Subscription) {
                inner.receivePost(subscription: subscription)
            }

            func receive(_ input: Input) -> Subscribers.Demand {
                return inner.receivePost(input)
            }

            func receive(completion: Subscribers.Completion<Failure>) {
                inner.receivePost(completion: completion)
            }

            var description: String { return inner.description }

            var customMirror: Mirror { return inner.customMirror }

            var playgroundDescription: Any { return description }
        }

        private enum State {
            case pendingPre
            case pre(Subscription)
            case pendingPost
            case post(Subscription)
            case cancelled
        }

        private let lock = UnfairLock.allocate()

        private var demand = Subscribers.Demand.none

        private var state = State.pendingPre

        private let downstream: Downstream

        private let handler: (Upstream.Failure) throws -> NewPublisher

        init(downstream: Downstream,
             handler: @escaping (Upstream.Failure) throws -> NewPublisher) {
            self.downstream = downstream
            self.handler = handler
        }

        deinit {
            lock.deallocate()
        }

        func receivePre(subscription: Subscription) {
            lock.lock()
            guard case .pendingPre = state else {
                lock.unlock()
                subscription.cancel()
                return
            }
            state = .pre(subscription)
            lock.unlock()
            downstream.receive(subscription: self)
        }

        func receivePre(_ input: Upstream.Output) -> Subscribers.Demand {
            lock.lock()
            demand -= 1
            lock.unlock()
            let newDemand = downstream.receive(input)
            lock.lock()
            demand += newDemand
            lock.unlock()
            return newDemand
        }

        func receivePre(completion: Subscribers.Completion<Upstream.Failure>) {
            switch completion {
            case .finished:
                lock.lock()
                switch state {
                case .pre:
                    state = .cancelled
                    lock.unlock()
                    downstream.receive(completion: .finished)
                case .pendingPre, .pendingPost, .post, .cancelled:
                    lock.unlock()
                }
            case .failure(let error):
                lock.lock()
                switch state {
                case .pre:
                    state = .pendingPost
                    lock.unlock()
                    do {
                        try handler(error).subscribe(CaughtS(inner: self))
                    } catch let anotherError {
                        lock.lock()
                        state = .cancelled
                        lock.unlock()
                        downstream.receive(completion: .failure(anotherError))
                    }
                case .cancelled:
                    lock.unlock()
                case .pendingPre, .post, .pendingPost:
                    completionBeforeSubscription()
                }
            }
        }

        func receivePost(subscription: Subscription) {
            lock.lock()
            guard case .pendingPost = state else {
                lock.unlock()
                subscription.cancel()
                return
            }
            state = .post(subscription)
            let demand = self.demand
            lock.unlock()
            if demand > 0 {
                subscription.request(demand)
            }
        }

        func receivePost(_ input: NewPublisher.Output) -> Subscribers.Demand {
            return downstream.receive(input)
        }

        func receivePost(completion: Subscribers.Completion<NewPublisher.Failure>) {
            lock.lock()
            guard case .post = state else {
                lock.unlock()
                return
            }
            state = .cancelled
            lock.unlock()
            downstream.receive(completion: completion.eraseError())
        }

        func request(_ demand: Subscribers.Demand) {
            demand.assertNonZero()
            lock.lock()
            switch state {
            case .pendingPre:
                // The client is only able to call the `request` method after we've sent
                // `self` downstream. We only do it in the `receivePre(subscription:)`
                // method, after setting `state` to `pre`.
                // After that `state` never becomes `pendingPre`.
                requestBeforeSubscription()
            case let .pre(subscription):
                self.demand += demand
                lock.unlock()
                subscription.request(demand)
            case .pendingPost:
                self.demand += demand
                lock.unlock()
            case let .post(subscription):
                lock.unlock()
                subscription.request(demand)
            case .cancelled:
                lock.unlock()
            }
        }

        func cancel() {
            lock.lock()
            switch state {
            case let .pre(subscription), let .post(subscription):
                state = .cancelled
                lock.unlock()
                subscription.cancel()
            case .pendingPre, .pendingPost, .cancelled:
                lock.unlock()
            }
        }

        var description: String { return "TryCatch" }

        var customMirror: Mirror {
            let children: [Mirror.Child] = [
                ("downstream", downstream),
                ("demand", demand)
            ]
            return Mirror(self, children: children)
        }

        var playgroundDescription: Any { return description }
    }
}

private func completionBeforeSubscription(file: StaticString = #file,
                                          line: UInt = #line) -> Never {
    fatalError("Unexpected state: received completion but do not have subscription",
               file: file,
               line: line)
}

private func requestBeforeSubscription(file: StaticString = #file,
                                       line: UInt = #line) -> Never {
    fatalError("Unexpected state: request before subscription sent",
               file: file,
               line: line)
}
