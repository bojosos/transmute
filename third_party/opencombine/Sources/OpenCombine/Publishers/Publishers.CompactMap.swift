//
//  Publishers.CompactMap.swift
//  
//
//  Created by Sergej Jaskiewicz on 11.07.2019.
//

extension Publisher {

    public func compactMap<ElementOfResult>(
        _ transform: @escaping (Output) -> ElementOfResult?
    ) -> Publishers.CompactMap<Self, ElementOfResult> {
        return .init(upstream: self, transform: transform)
    }

    public func tryCompactMap<ElementOfResult>(
        _ transform: @escaping (Output) throws -> ElementOfResult?
    ) -> Publishers.TryCompactMap<Self, ElementOfResult> {
        return .init(upstream: self, transform: transform)
    }
}

extension Publishers.CompactMap {

    public func compactMap<ElementOfResult>(
        _ transform: @escaping (Output) -> ElementOfResult?
    ) -> Publishers.CompactMap<Upstream, ElementOfResult> {
        return .init(upstream: upstream,
                     transform: { self.transform($0).flatMap(transform) })
    }

    public func map<ElementOfResult>(
        _ transform: @escaping (Output) -> ElementOfResult
    ) -> Publishers.CompactMap<Upstream, ElementOfResult> {
        return .init(upstream: upstream,
                     transform: { self.transform($0).map(transform) })
    }
}

extension Publishers.TryCompactMap {

    public func compactMap<ElementOfResult>(
        _ transform: @escaping (Output) throws -> ElementOfResult?
    ) -> Publishers.TryCompactMap<Upstream, ElementOfResult> {
        return .init(upstream: upstream,
                     transform: { try self.transform($0).flatMap(transform) })
    }
}

extension Publishers {

    public struct CompactMap<Upstream: Publisher, Output>: Publisher {

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let transform: (Upstream.Output) -> Output?

        public init(upstream: Upstream,
                    transform: @escaping (Upstream.Output) -> Output?) {
            self.upstream = upstream
            self.transform = transform
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Input == Output, Downstream.Failure == Failure
        {
            upstream.subscribe(Inner(downstream: subscriber, filter: transform))
        }
    }

    public struct TryCompactMap<Upstream: Publisher, Output>: Publisher {

        public typealias Failure = Error

        public let upstream: Upstream

        public let transform: (Upstream.Output) throws -> Output?

        public init(upstream: Upstream,
                    transform: @escaping (Upstream.Output) throws -> Output?) {
            self.upstream = upstream
            self.transform = transform
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Input == Output, Downstream.Failure == Failure
        {
            upstream.subscribe(Inner(downstream: subscriber, filter: transform))
        }
    }
}

extension Publishers.CompactMap {
    private struct Inner<Downstream: Subscriber>
        : Subscriber,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Upstream.Failure == Downstream.Failure
    {
        typealias Input = Upstream.Output
        typealias Failure = Upstream.Failure

        private let downstream: Downstream
        private let filter: (Input) -> Downstream.Input?

        let combineIdentifier = CombineIdentifier()

        init(downstream: Downstream, filter: @escaping (Input) -> Downstream.Input?) {
            self.downstream = downstream
            self.filter = filter
        }

        func receive(subscription: Subscription) {
            downstream.receive(subscription: subscription)
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            if let output = filter(input) {
                return downstream.receive(output)
            }
            return .max(1)
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            downstream.receive(completion: completion)
        }

        var description: String { return "CompactMap" }

        var customMirror: Mirror {
            return Mirror(self, children: EmptyCollection())
        }

        var playgroundDescription: Any { return description }
    }
}

extension Publishers.TryCompactMap {
    private final class Inner<Downstream: Subscriber>
        : FilterProducer<Downstream,
                         Upstream.Output,
                         Output,
                         Upstream.Failure,
                         (Upstream.Output) throws -> Output?>
        where Downstream.Failure == Error, Downstream.Input == Output
    {
        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Output?, Error> {
            do {
                return try .continue(filter(newValue))
            } catch {
                return .failure(error)
            }
        }

        override var description: String { return "TryCompactMap" }
    }
}
