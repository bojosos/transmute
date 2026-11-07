//
//  Publishers.SetFailureType.swift
//  
//
//  Created by Sergej Jaskiewicz on 08.07.2019.
//

extension Publishers {

    public struct SetFailureType<Upstream: Publisher, Failure: Error>: Publisher
        where Upstream.Failure == Never
    {
        public typealias Output = Upstream.Output

        public let upstream: Upstream

        public init(upstream: Upstream) {
            self.upstream = upstream
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Failure, Downstream.Input == Output
        {
            upstream.subscribe(Inner(downstream: subscriber))
        }

        public func setFailureType<NewFailure: Error>(
            to failure: NewFailure.Type
        ) -> Publishers.SetFailureType<Upstream, NewFailure> {
            return .init(upstream: upstream)
        }
    }
}

extension Publishers.SetFailureType: Equatable where Upstream: Equatable {}

extension Publisher where Failure == Never {

    public func setFailureType<NewFailure: Error>(
        to failureType: NewFailure.Type
    ) -> Publishers.SetFailureType<Self, NewFailure> {
        return .init(upstream: self)
    }
}

extension Publishers.SetFailureType {
    private struct Inner<Downstream: Subscriber>
        : Subscriber,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Upstream.Output == Downstream.Input, Failure == Downstream.Failure
    {
        private let downstream: Downstream
        let combineIdentifier = CombineIdentifier()

        fileprivate init(downstream: Downstream) {
            self.downstream = downstream
        }

        func receive(subscription: Subscription) {
            downstream.receive(subscription: subscription)
        }

        func receive(_ input: Upstream.Output) -> Subscribers.Demand {
            return downstream.receive(input)
        }

        func receive(completion: Subscribers.Completion<Never>) {
            downstream.receive(completion: .finished)
        }

        var description: String { return "SetFailureType" }

        var customMirror: Mirror {
            return Mirror(self, children: EmptyCollection())
        }

        var playgroundDescription: Any { return description }
    }
}
