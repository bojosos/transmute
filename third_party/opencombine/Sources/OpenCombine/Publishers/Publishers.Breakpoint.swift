//
//  Publishers.Breakpoint.swift
//  
//
//  Created by Sergej Jaskiewicz on 03.12.2019.
//

#if !WASI

#if canImport(COpenCombineHelpers)
import COpenCombineHelpers
#endif

extension Publisher {

    public func breakpoint(
        receiveSubscription: ((Subscription) -> Bool)? = nil,
        receiveOutput: ((Output) -> Bool)? = nil,
        receiveCompletion: ((Subscribers.Completion<Failure>) -> Bool)? = nil
    ) -> Publishers.Breakpoint<Self> {
        return .init(upstream: self,
                     receiveSubscription: receiveSubscription,
                     receiveOutput: receiveOutput,
                     receiveCompletion: receiveCompletion)
    }

    public func breakpointOnError() -> Publishers.Breakpoint<Self> {
        return breakpoint(receiveCompletion: { completion in
            switch completion {
            case .finished:
                return false
            case .failure:
                return true
            }
        })
    }
}

extension Publishers {

    public struct Breakpoint<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let receiveSubscription: ((Subscription) -> Bool)?

        public let receiveOutput: ((Upstream.Output) -> Bool)?

        public let receiveCompletion:
            ((Subscribers.Completion<Upstream.Failure>) -> Bool)?

        public init(
            upstream: Upstream,
            receiveSubscription: ((Subscription) -> Bool)? = nil,
            receiveOutput: ((Upstream.Output) -> Bool)? = nil,
            receiveCompletion: ((Subscribers.Completion<Failure>) -> Bool)? = nil
        ) {
            self.upstream = upstream
            self.receiveSubscription = receiveSubscription
            self.receiveOutput = receiveOutput
            self.receiveCompletion = receiveCompletion
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure,
                  Upstream.Output == Downstream.Input
        {
            upstream.subscribe(Inner(self, downstream: subscriber))
        }
    }
}

extension Publishers.Breakpoint {
    private struct Inner<Downstream: Subscriber>
        : Subscriber,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Downstream.Input == Upstream.Output, Downstream.Failure == Upstream.Failure
    {
        typealias Input = Upstream.Output
        typealias Failure = Upstream.Failure

        private let downstream: Downstream
        private let breakpoint: Publishers.Breakpoint<Upstream>

        let combineIdentifier = CombineIdentifier()

        init(_ breakpoint: Publishers.Breakpoint<Upstream>,
             downstream: Downstream) {
            self.downstream = downstream
            self.breakpoint = breakpoint
        }

        func receive(subscription: Subscription) {
            if breakpoint.receiveSubscription?(subscription) == true {
                __stopInDebugger()
            }
            downstream.receive(subscription: subscription)
        }

        func receive(_ input: Upstream.Output) -> Subscribers.Demand {
            if breakpoint.receiveOutput?(input) == true {
                __stopInDebugger()
            }
            return downstream.receive(input)
        }

        func receive(completion: Subscribers.Completion<Upstream.Failure>) {
            if breakpoint.receiveCompletion?(completion) == true {
                __stopInDebugger()
            }
            downstream.receive(completion: completion)
        }

        var description: String { return "Breakpoint" }

        var customMirror: Mirror {
            return Mirror(self, children: EmptyCollection())
        }

        var playgroundDescription: Any { return description }
    }
}

#endif // !WASI
