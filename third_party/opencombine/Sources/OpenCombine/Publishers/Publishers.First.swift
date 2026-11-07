//
//  Publishers.First.swift
//  
//
//  Created by Joseph Spadafora on 7/8/19.
//

extension Publisher {

    public func first() -> Publishers.First<Self> {
        return .init(upstream: self)
    }


    public func first(
        where predicate: @escaping (Output) -> Bool
    ) -> Publishers.FirstWhere<Self> {
        return .init(upstream: self, predicate: predicate)
    }


    public func tryFirst(
        where predicate: @escaping (Output) throws -> Bool
    ) -> Publishers.TryFirstWhere<Self> {
        return .init(upstream: self, predicate: predicate)
    }
}

extension Publishers {

    public struct First<Upstream: Publisher>: Publisher {

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

    public struct FirstWhere<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let predicate: (Output) -> Bool

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

    public struct TryFirstWhere<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Error

        public let upstream: Upstream

        public let predicate: (Output) throws -> Bool

        public init(upstream: Upstream, predicate: @escaping (Output) throws -> Bool) {
            self.upstream = upstream
            self.predicate = predicate
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Failure == Downstream.Failure, Output == Downstream.Input
        {
            upstream.subscribe(Inner(downstream: subscriber, predicate: predicate))
        }
    }
}

extension Publishers.First: Equatable where Upstream: Equatable {}

extension Publishers.First {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         Void>
        where Upstream.Output == Downstream.Input,
              Upstream.Failure == Downstream.Failure
    {
        fileprivate init(downstream: Downstream) {
            super.init(downstream: downstream, initial: nil, reduce: ())
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            result = newValue
            return .finished
        }

        override var description: String { return "First" }
    }
}

extension Publishers.FirstWhere {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream, Output, Output, Failure, (Output) -> Bool>
        where Upstream.Output == Downstream.Input,
              Upstream.Failure == Downstream.Failure
    {
        fileprivate init(downstream: Downstream, predicate: @escaping (Output) -> Bool) {
            super.init(downstream: downstream, initial: nil, reduce: predicate)
        }

        override func receive(
            newValue: Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            if reduce(newValue) {
                result = newValue
                return .finished
            } else {
                return .continue
            }
        }

        override var description: String { return "TryFirst" }
    }
}

extension Publishers.TryFirstWhere {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Output,
                         Output,
                         Upstream.Failure,
                         (Output) throws -> Bool>
        where Upstream.Output == Downstream.Input, Downstream.Failure == Error
    {
        fileprivate init(downstream: Downstream,
                         predicate: @escaping (Output) throws -> Bool) {
            super.init(downstream: downstream, initial: nil, reduce: predicate)
        }

        override func receive(
            newValue: Output
        ) -> PartialCompletion<Void, Error> {
            do {
                if try reduce(newValue) {
                    result = newValue
                    return .finished
                } else {
                    return .continue
                }
            } catch {
                return .failure(error)
            }
        }

        override var description: String { return "TryFirstWhere" }
    }
}
