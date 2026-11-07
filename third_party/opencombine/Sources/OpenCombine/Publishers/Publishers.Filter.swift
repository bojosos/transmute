//
//  Publishers.Filter.swift
//  
//
//  Created by Joseph Spadafora on 7/3/19.
//

extension Publisher {

    public func filter(
        _ isIncluded: @escaping (Output) -> Bool
    ) -> Publishers.Filter<Self> {
        return Publishers.Filter(upstream: self, isIncluded: isIncluded)
    }

    public func tryFilter(
        _ isIncluded: @escaping (Output) throws -> Bool
    ) -> Publishers.TryFilter<Self> {
        return Publishers.TryFilter(upstream: self, isIncluded: isIncluded)
    }
}

extension Publishers.Filter {

    public func filter(
        _ isIncluded: @escaping (Output) -> Bool
    ) -> Publishers.Filter<Upstream> {
        return .init(upstream: upstream) { self.isIncluded($0) && isIncluded($0) }
    }

    public func tryFilter(
        _ isIncluded: @escaping (Output) throws -> Bool
    ) -> Publishers.TryFilter<Upstream> {
        return .init(upstream: upstream) { try self.isIncluded($0) && isIncluded($0) }
    }
}

extension Publishers.TryFilter {

    public func filter(
        _ isIncluded: @escaping (Output) -> Bool
    ) -> Publishers.TryFilter<Upstream> {
        return .init(upstream: upstream) { try self.isIncluded($0) && isIncluded($0) }
    }

    public func tryFilter(
        _ isIncluded: @escaping (Output) throws -> Bool
    ) -> Publishers.TryFilter<Upstream> {
        return .init(upstream: upstream) { try self.isIncluded($0) && isIncluded($0) }
    }
}

extension Publishers {

    public struct Filter<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let isIncluded: (Upstream.Output) -> Bool

        public init(upstream: Upstream, isIncluded: @escaping (Output) -> Bool) {
            self.upstream = upstream
            self.isIncluded = isIncluded
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure,
                  Upstream.Output == Downstream.Input
        {
            upstream.subscribe(Inner(downstream: subscriber, filter: isIncluded))
        }
    }

    public struct TryFilter<Upstream>: Publisher where Upstream: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Error

        public let upstream: Upstream

        public let isIncluded: (Upstream.Output) throws -> Bool

        public init(upstream: Upstream,
                    isIncluded: @escaping (Upstream.Output) throws -> Bool) {
            self.upstream = upstream
            self.isIncluded = isIncluded
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Output == Downstream.Input,
                  Downstream.Failure == Failure
        {
            upstream.subscribe(Inner(downstream: subscriber, filter: isIncluded))
        }
    }
}

extension Publishers.Filter {
    private struct Inner<Downstream: Subscriber>
        : Subscriber,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Upstream.Output == Downstream.Input, Upstream.Failure == Downstream.Failure
    {
        typealias Input = Upstream.Output
        typealias Failure = Upstream.Failure

        private let downstream: Downstream
        private let filter: (Input) -> Bool

        let combineIdentifier = CombineIdentifier()

        init(downstream: Downstream, filter: @escaping (Input) -> Bool) {
            self.downstream = downstream
            self.filter = filter
        }

        func receive(subscription: Subscription) {
            downstream.receive(subscription: subscription)
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            if filter(input) {
                return downstream.receive(input)
            }
            return .max(1)
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            downstream.receive(completion: completion)
        }

        var description: String { return "Filter" }

        var customMirror: Mirror {
            return Mirror(self, children: EmptyCollection())
        }

        var playgroundDescription: Any { return description }
    }
}

extension Publishers.TryFilter {
    private final class Inner<Downstream: Subscriber>
        : FilterProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Upstream.Output) throws -> Bool>
        where Downstream.Input == Upstream.Output, Downstream.Failure == Error
    {
        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Upstream.Output?, Error> {
            do {
                return try filter(newValue) ? .continue(newValue) : .continue(nil)
            } catch {
                return .failure(error)
            }
        }

        override var description: String { return "TryFilter" }
    }
}
