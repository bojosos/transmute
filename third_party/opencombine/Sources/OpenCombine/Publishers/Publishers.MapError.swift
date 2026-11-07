//
//  Publishers.MapError.swift
//  
//
//  Created by Joseph Spadafora on 7/4/19.
//

extension Publishers {

    public struct MapError<Upstream: Publisher, Failure: Error>: Publisher {

        public typealias Output = Upstream.Output

        public let upstream: Upstream

        public let transform: (Upstream.Failure) -> Failure

        public init(upstream: Upstream, _ map: @escaping (Upstream.Failure) -> Failure) {
            self.upstream = upstream
            self.transform = map
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Failure == Downstream.Failure,
                  Upstream.Output == Downstream.Input
        {
            upstream.subscribe(Inner(downstream: subscriber, map: transform))
        }
    }
}

extension Publisher {

    public func mapError<NewFailure: Error>(
        _ transform: @escaping (Failure) -> NewFailure
    ) -> Publishers.MapError<Self, NewFailure>
    {
        return Publishers.MapError(upstream: self, transform)
    }
}

extension Publishers.MapError {

    private struct Inner<Downstream: Subscriber>
        : Subscriber,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Upstream.Output == Downstream.Input
    {
        typealias Input = Upstream.Output
        typealias Failure = Upstream.Failure

        private let downstream: Downstream
        private let map: (Upstream.Failure) -> Downstream.Failure

        let combineIdentifier = CombineIdentifier()

        var description: String { return "MapError" }

        var customMirror: Mirror { return Mirror(self, children: EmptyCollection()) }

        var playgroundDescription: Any { return description }

        init(downstream: Downstream,
             map: @escaping (Upstream.Failure) -> Downstream.Failure) {
            self.downstream = downstream
            self.map = map
        }

        func receive(subscription: Subscription) {
            downstream.receive(subscription: subscription)
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            return downstream.receive(input)
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            switch completion {
            case .finished:
                downstream.receive(completion: .finished)
            case .failure(let error):
                downstream.receive(completion: .failure(map(error)))
            }
        }
    }
}
