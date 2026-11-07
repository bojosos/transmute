//
//  Publishers.AllSatisfy.swift
//  
//
//  Created by Sergej Jaskiewicz on 09.10.2019.
//

extension Publisher {

    public func allSatisfy(
        _ predicate: @escaping (Output) -> Bool
    ) -> Publishers.AllSatisfy<Self> {
        return .init(upstream: self, predicate: predicate)
    }

    public func tryAllSatisfy(
        _ predicate: @escaping (Output) throws -> Bool
    ) -> Publishers.TryAllSatisfy<Self> {
        return .init(upstream: self, predicate: predicate)
    }
}

extension Publishers {

    public struct AllSatisfy<Upstream: Publisher>: Publisher {

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

    public struct TryAllSatisfy<Upstream: Publisher>: Publisher {

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

extension Publishers.AllSatisfy {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Bool,
                         Upstream.Failure,
                         (Upstream.Output) -> Bool>
        where Downstream.Input == Output, Upstream.Failure == Downstream.Failure
    {
        fileprivate init(downstream: Downstream,
                         predicate: @escaping (Upstream.Output) -> Bool) {
            super.init(downstream: downstream, initial: true, reduce: predicate)
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            if !reduce(newValue) {
                result = false
                return .finished
            }

            return .continue
        }

        override var description: String { return "AllSatisfy" }
    }
}

extension Publishers.TryAllSatisfy {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Bool,
                         Upstream.Failure,
                         (Upstream.Output) throws -> Bool>
        where Downstream.Input == Output, Downstream.Failure == Error
    {
        fileprivate init(downstream: Downstream,
                         predicate: @escaping (Upstream.Output) throws -> Bool) {
            super.init(downstream: downstream, initial: true, reduce: predicate)
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            do {
                if try !reduce(newValue) {
                    result = false
                    return .finished
                }
            } catch {
                return .failure(error)
            }

            return .continue
        }

        override var description: String { return "TryAllSatisfy" }
    }
}
