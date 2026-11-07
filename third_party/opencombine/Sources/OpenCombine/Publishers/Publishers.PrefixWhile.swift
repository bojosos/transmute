//
//  Publishers.PrefixWhile.swift
//  
//
//  Created by Sergej Jaskiewicz on 24.10.2019.
//

extension Publisher {

    public func prefix(
        while predicate: @escaping (Output) -> Bool
    ) -> Publishers.PrefixWhile<Self> {
        return .init(upstream: self, predicate: predicate)
    }

    public func tryPrefix(
        while predicate: @escaping (Output) throws -> Bool
    ) -> Publishers.TryPrefixWhile<Self> {
        return .init(upstream: self, predicate: predicate)
    }
}

extension Publishers {

    public struct PrefixWhile<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let predicate: (Upstream.Output) -> Bool

        public init(upstream: Upstream, predicate: @escaping (Upstream.Output) -> Bool) {
            self.upstream = upstream
            self.predicate = predicate
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure,
                  Upstream.Output == Downstream.Input
        {
            upstream.subscribe(Inner(downstream: subscriber, filter: predicate))
        }
    }

    public struct TryPrefixWhile<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Error

        public let upstream: Upstream

        public let predicate: (Upstream.Output) throws -> Bool

        public init(upstream: Upstream,
                    predicate: @escaping (Upstream.Output) throws -> Bool) {
            self.upstream = upstream
            self.predicate = predicate
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Output == Downstream.Input, Downstream.Failure == Error
        {
            upstream.subscribe(Inner(downstream: subscriber, filter: predicate))
        }
    }
}

extension Publishers.PrefixWhile {
    private final class Inner<Downstream: Subscriber>
        : FilterProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Upstream.Output) -> Bool>
        where Downstream.Input == Upstream.Output, Downstream.Failure == Upstream.Failure
    {
        typealias Input = Upstream.Output

        typealias Failure = Upstream.Failure

        override func receive(
            newValue: Input
        ) -> PartialCompletion<Upstream.Output?, Downstream.Failure> {
            return filter(newValue) ? .continue(newValue) : .finished
        }

        override var description: String { return "PrefixWhile" }
    }
}

extension Publishers.TryPrefixWhile {
    private final class Inner<Downstream: Subscriber>
        : FilterProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Upstream.Output) throws -> Bool>
        where Downstream.Input == Upstream.Output, Downstream.Failure == Error
    {
        typealias Input = Upstream.Output

        typealias Failure = Upstream.Failure

        override func receive(
            newValue: Input
        ) -> PartialCompletion<Upstream.Output?, Downstream.Failure> {
            do {
                return try filter(newValue) ? .continue(newValue) : .finished
            } catch {
                return .failure(error)
            }
        }

        override var description: String { return "TryPrefixWhile" }
    }
}
