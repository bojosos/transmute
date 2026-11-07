//
//  Publishers.Reduce.swift
//  
//
//  Created by Sergej Jaskiewicz on 09.10.2019.
//

extension Publisher {

    public func reduce<Accumulator>(
        _ initialResult: Accumulator,
        _ nextPartialResult: @escaping (Accumulator, Output) -> Accumulator
    ) -> Publishers.Reduce<Self, Accumulator> {
        return .init(upstream: self,
                     initial: initialResult,
                     nextPartialResult: nextPartialResult)
    }


    public func tryReduce<Accumulator>(
        _ initialResult: Accumulator,
        _ nextPartialResult: @escaping (Accumulator, Output) throws -> Accumulator
    ) -> Publishers.TryReduce<Self, Accumulator> {
        return .init(upstream: self,
                     initial: initialResult,
                     nextPartialResult: nextPartialResult)
    }
}

extension Publishers {

    public struct Reduce<Upstream: Publisher, Output>: Publisher {

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let initial: Output

        public let nextPartialResult: (Output, Upstream.Output) -> Output

        public init(upstream: Upstream,
                    initial: Output,
                    nextPartialResult: @escaping (Output, Upstream.Output) -> Output) {
            self.upstream = upstream
            self.initial = initial
            self.nextPartialResult = nextPartialResult
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Output == Downstream.Input, Upstream.Failure == Downstream.Failure
        {
            let inner = Inner(downstream: subscriber,
                              initial: initial,
                              reduce: nextPartialResult)
            upstream.subscribe(inner)
        }
    }

    public struct TryReduce<Upstream: Publisher, Output>: Publisher {

        public typealias Failure = Error

        public let upstream: Upstream

        public let initial: Output

        public let nextPartialResult: (Output, Upstream.Output) throws -> Output

        public init(
            upstream: Upstream,
            initial: Output,
            nextPartialResult: @escaping (Output, Upstream.Output) throws -> Output
        ) {
            self.upstream = upstream
            self.initial = initial
            self.nextPartialResult = nextPartialResult
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Output == Downstream.Input, Downstream.Failure == Error
        {
            let inner = Inner(downstream: subscriber,
                              initial: initial,
                              reduce: nextPartialResult)
            upstream.subscribe(inner)
        }
    }
}

extension Publishers.Reduce {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Output,
                         Upstream.Failure,
                         (Output, Upstream.Output) -> Output>
        where Downstream.Input == Output, Upstream.Failure == Downstream.Failure
    {
        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            result = reduce(result!, newValue)
            return .continue
        }

        override var description: String { return "Reduce" }
    }
}

extension Publishers.TryReduce {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Output,
                         Upstream.Failure,
                         (Output, Upstream.Output) throws -> Output>
        where Downstream.Input == Output, Downstream.Failure == Error
    {
        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            do {
                result = try reduce(result!, newValue)
                return .continue
            } catch {
                return .failure(error)
            }
        }

        override var description: String { return "TryReduce" }
    }
}
