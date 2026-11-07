//
//  Publishers.AssertNoFailure.swift
//  
//
//  Created by Sergej Jaskiewicz on 25.12.2019.
//

extension Publisher {

    public func assertNoFailure(_ prefix: String = "",
                                file: StaticString = #file,
                                line: UInt = #line) -> Publishers.AssertNoFailure<Self> {
        return .init(upstream: self, prefix: prefix, file: file, line: line)
    }
}

extension Publishers {

    public struct AssertNoFailure<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Never

        public let upstream: Upstream

        public let prefix: String

        public let file: StaticString

        public let line: UInt

        public init(upstream: Upstream, prefix: String, file: StaticString, line: UInt) {
            self.upstream = upstream
            self.prefix = prefix
            self.file = file
            self.line = line
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Input == Output, Downstream.Failure == Never
        {
            upstream.subscribe(Inner(downstream: subscriber,
                                     prefix: prefix,
                                     file: file,
                                     line: line))
        }
    }
}

extension Publishers.AssertNoFailure {
    private struct Inner<Downstream: Subscriber>
        : Subscriber,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
        where Downstream.Input == Upstream.Output, Downstream.Failure == Never
    {
        typealias Input = Upstream.Output

        typealias Failure = Upstream.Failure

        private let downstream: Downstream

        private let prefix: String

        private let file: StaticString

        private let line: UInt

        let combineIdentifier = CombineIdentifier()

        init(downstream: Downstream, prefix: String, file: StaticString, line: UInt) {
            self.downstream = downstream
            self.prefix = prefix
            self.file = file
            self.line = line
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
                let prefix = self.prefix.isEmpty ? "" : self.prefix + ": "
                fatalError("\(prefix)\(error)", file: file, line: line)
            }
        }

        var description: String { return "AssertNoFailure" }

        var customMirror: Mirror {
            let children: [Mirror.Child] = [
                ("file", file),
                ("line", line),
                ("prefix", prefix)
            ]
            return Mirror(self, children: children)
        }

        var playgroundDescription: Any { return description }
    }
}
