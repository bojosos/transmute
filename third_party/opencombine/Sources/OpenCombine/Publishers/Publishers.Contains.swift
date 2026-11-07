//
//  Publishers.Contains.swift
//  
//
//  Created by Sergej Jaskiewicz on 09.10.2019.
//

extension Publisher where Output: Equatable {

    public func contains(_ output: Output) -> Publishers.Contains<Self> {
        return .init(upstream: self, output: output)
    }
}

extension Publisher {

    public func contains(
        where predicate: @escaping (Output) -> Bool
    ) -> Publishers.ContainsWhere<Self> {
        return .init(upstream: self, predicate: predicate)
    }

    public func tryContains(
        where predicate: @escaping (Output) throws -> Bool
    ) -> Publishers.TryContainsWhere<Self> {
        return .init(upstream: self, predicate: predicate)
    }
}

extension Publishers {

    public struct Contains<Upstream: Publisher>: Publisher
        where Upstream.Output: Equatable
    {
        public typealias Output = Bool

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let output: Upstream.Output

        public init(upstream: Upstream, output: Upstream.Output) {
            self.upstream = upstream
            self.output = output
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure, Downstream.Input == Bool
        {
            upstream.subscribe(Inner(downstream: subscriber, output: output))
        }
    }

    public struct ContainsWhere<Upstream: Publisher>: Publisher {

        public typealias Output = Bool

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let predicate: (Upstream.Output) -> Bool

        public init(upstream: Upstream, predicate: @escaping (Upstream.Output) -> Bool) {
            self.upstream = upstream
            self.predicate = predicate
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure, Downstream.Input == Bool
        {
            upstream.subscribe(Inner(downstream: subscriber, predicate: predicate))
        }
    }

    public struct TryContainsWhere<Upstream: Publisher>: Publisher {

        public typealias Output = Bool

        public typealias Failure = Error

        public let upstream: Upstream

        public let predicate: (Upstream.Output) throws -> Bool

        public init(upstream: Upstream,
                    predicate: @escaping (Upstream.Output) throws -> Bool) {
            self.upstream = upstream
            self.predicate = predicate
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Error, Downstream.Input == Bool
        {
            upstream.subscribe(Inner(downstream: subscriber, predicate: predicate))
        }
    }
}

extension Publishers.Contains {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream, Upstream.Output, Bool, Upstream.Failure, Void>
        where Upstream.Failure == Downstream.Failure, Downstream.Input == Bool
    {
        private let output: Upstream.Output

        fileprivate init(downstream: Downstream, output: Upstream.Output) {
            self.output = output
            super.init(downstream: downstream, initial: false, reduce: ())
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            if newValue == output {
                result = true
                return .finished
            }

            return .continue
        }

        override var description: String { return "Contains" }
    }
}

extension Publishers.Contains : Equatable where Upstream: Equatable {}

extension Publishers.ContainsWhere {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output, Bool,
                         Upstream.Failure,
                         (Upstream.Output) -> Bool>
        where Upstream.Failure == Downstream.Failure, Downstream.Input == Bool
    {
        fileprivate init(downstream: Downstream,
                         predicate: @escaping (Upstream.Output) -> Bool) {
            super.init(downstream: downstream, initial: false, reduce: predicate)
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            if reduce(newValue) {
                result = true
                return .finished
            }

            return .continue
        }

        override var description: String { return "ContainsWhere" }
    }
}

extension Publishers.TryContainsWhere {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output, Bool,
                         Upstream.Failure,
                         (Upstream.Output) throws -> Bool>
        where Downstream.Failure == Error, Downstream.Input == Bool
    {
        fileprivate init(downstream: Downstream,
                         predicate: @escaping (Upstream.Output) throws -> Bool) {
            super.init(downstream: downstream, initial: false, reduce: predicate)
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            do {
                if try reduce(newValue) {
                    result = true
                    return .finished
                }
            } catch {
                return .failure(error)
            }

            return .continue
        }

        override var description: String { return "TryContainsWhere" }
    }
}
