//
//  Publishers.Last.swift
//  
//
//  Created by Joseph Spadafora on 7/9/19.
//

extension Publisher {

    public func last() -> Publishers.Last<Self> {
        return .init(upstream: self)
    }

    public func last(
        where predicate: @escaping (Output) -> Bool
    ) -> Publishers.LastWhere<Self> {
        return .init(upstream: self, predicate: predicate)
    }

    public func tryLast(
        where predicate: @escaping (Output) throws -> Bool
    ) -> Publishers.TryLastWhere<Self> {
        return .init(upstream: self, predicate: predicate)
    }
}

extension Publishers {

    public struct Last<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public init(upstream: Upstream) {
            self.upstream = upstream
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Failure == Downstream.Failure, Output == Downstream.Input
        {
            upstream.subscribe(Inner(downstream: subscriber))
        }
    }

    public struct LastWhere<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let predicate: (Upstream.Output) -> Bool

        public init(upstream: Upstream, predicate: @escaping (Output) -> Bool) {
            self.upstream = upstream
            self.predicate = predicate
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Failure == Downstream.Failure, Output == Downstream.Input
        {
            upstream.subscribe(Inner(downstream: subscriber, predicate: predicate))
        }
    }

    public struct TryLastWhere<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Error

        public let upstream: Upstream

        public let predicate: (Upstream.Output) throws -> Bool

        public init(upstream: Upstream, predicate: @escaping (Output) throws -> Bool) {
            self.upstream = upstream
            self.predicate = predicate
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Error, Output == Downstream.Input
        {
            upstream.subscribe(Inner(downstream: subscriber, predicate: predicate))
        }
    }
}

extension Publishers.Last: Equatable where Upstream: Equatable {}

extension Publishers.Last {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         Void>
        where Upstream.Output == Downstream.Input, Upstream.Failure == Downstream.Failure
    {
        fileprivate init(downstream: Downstream) {
            super.init(downstream: downstream, initial: nil, reduce: ())
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            result = newValue
            return .continue
        }

        override var description: String { return "Last" }
    }
}

extension Publishers.LastWhere {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Upstream.Output) -> Bool>
        where Upstream.Output == Downstream.Input, Upstream.Failure == Downstream.Failure
    {
        fileprivate init(downstream: Downstream,
                         predicate: @escaping (Upstream.Output) -> Bool) {
            super.init(downstream: downstream, initial: nil, reduce: predicate)
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            if reduce(newValue) {
                result = newValue
            }
            return .continue
        }

        override var description: String { return "LastWhere" }
    }
}

extension Publishers.TryLastWhere {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Upstream.Output) throws -> Bool>
        where Upstream.Output == Downstream.Input, Downstream.Failure == Error
    {
        fileprivate init(downstream: Downstream,
                         predicate: @escaping (Upstream.Output) throws -> Bool) {
            super.init(downstream: downstream, initial: nil, reduce: predicate)
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            do {
                if try reduce(newValue) {
                    result = newValue
                }
                return .continue
            } catch {
                return .failure(error)
            }
        }

        override var description: String { return "TryLastWhere" }
    }
}
