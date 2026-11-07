//
//  Publishers.RemoveDuplicates.swift
//  
//
//  Created by Sergej Jaskiewicz on 24.10.2019.
//

extension Publisher where Output: Equatable {

    public func removeDuplicates() -> Publishers.RemoveDuplicates<Self> {
        return removeDuplicates(by: ==)
    }
}

extension Publisher {

    public func removeDuplicates(
        by predicate: @escaping (Output, Output) -> Bool
    ) -> Publishers.RemoveDuplicates<Self> {
        return .init(upstream: self, predicate: predicate)
    }

    public func tryRemoveDuplicates(
        by predicate: @escaping (Output, Output) throws -> Bool
    ) -> Publishers.TryRemoveDuplicates<Self> {
        return .init(upstream: self, predicate: predicate)
    }
}

extension Publishers {

    public struct RemoveDuplicates<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let predicate: (Output, Output) -> Bool

        public init(upstream: Upstream, predicate: @escaping (Output, Output) -> Bool) {
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

    public struct TryRemoveDuplicates<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Error

        public let upstream: Upstream

        public let predicate: (Output, Output) throws -> Bool

        public init(upstream: Upstream,
                    predicate: @escaping (Output, Output) throws -> Bool) {
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

extension Publishers.RemoveDuplicates {
    private final class Inner<Downstream: Subscriber>
        : FilterProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Output, Output) -> Bool>
        where Downstream.Input == Upstream.Output, Downstream.Failure == Upstream.Failure
    {
        private var last: Upstream.Output?

        override func receive(
            newValue: Input
        ) -> PartialCompletion<Upstream.Output?, Downstream.Failure> {
            let last = self.last
            self.last = newValue
            return last.map {
                filter($0, newValue) ? .continue(nil) : .continue(newValue)
            } ?? .continue(newValue)
        }

        override var description: String { return "RemoveDuplicates" }

        override var customMirror: Mirror {
            let children: [Mirror.Child] = [
                ("downstream", downstream),
                ("last", last as Any)
            ]
            return Mirror(self, children: children)
        }
    }
}

extension Publishers.TryRemoveDuplicates {
    private final class Inner<Downstream: Subscriber>
        : FilterProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Output, Output) throws -> Bool>
        where Downstream.Input == Upstream.Output, Downstream.Failure == Error
    {
        private var last: Upstream.Output?

        override func receive(
            newValue: Input
        ) -> PartialCompletion<Upstream.Output?, Downstream.Failure> {
            let last = self.last
            self.last = newValue
            return last.map {
                do {
                    return try filter($0, newValue)
                        ? .continue(nil)
                        : .continue(newValue)
                } catch {
                    return .failure(error)
                }
            } ?? .continue(newValue)
        }

        override var description: String { return "TryRemoveDuplicates" }

        override var customMirror: Mirror {
            let children: [Mirror.Child] = [
                ("downstream", downstream),
                ("last", last as Any)
            ]
            return Mirror(self, children: children)
        }
    }
}
