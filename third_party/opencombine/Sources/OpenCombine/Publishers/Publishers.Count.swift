//
//  Publishers.Count.swift
//
//
//  Created by Joseph Spadafora on 6/25/19.
//

extension Publisher {

    public func count() -> Publishers.Count<Self> {
        return Publishers.Count(upstream: self)
    }
}

extension Publishers {

    public struct Count<Upstream: Publisher>: Publisher {

        public typealias Output = Int

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public init(upstream: Upstream) {
            self.upstream = upstream
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure,
                  Downstream.Input == Output
        {
            upstream.subscribe(Inner(downstream: subscriber))
        }
    }
}

extension Publishers.Count: Equatable where Upstream: Equatable {}

extension Publishers.Count {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream, Upstream.Output, Int, Failure, Void>
        where Downstream.Input == Int,
              Upstream.Failure == Downstream.Failure
    {
        fileprivate init(downstream: Downstream) {
            super.init(downstream: downstream, initial: 0, reduce: ())
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            result! += 1
            return .continue
        }

        override var description: String { return "Count" }
    }
}
