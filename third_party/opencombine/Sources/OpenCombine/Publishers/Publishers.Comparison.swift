//
//  Publishers.Comparison.swift
//  OpenCombine
//
//  Created by Ilija Puaca on 22/7/19.
//

extension Publisher where Output: Comparable {

    public func min() -> Publishers.Comparison<Self> {
        return max(by: >)
    }

    public func max() -> Publishers.Comparison<Self> {
        return max(by: <)
    }
}

extension Publisher {

    public func min(
        by areInIncreasingOrder: @escaping (Output, Output) -> Bool
    ) -> Publishers.Comparison<Self> {
        return max(by: { areInIncreasingOrder($1, $0) })
    }

    public func tryMin(
        by areInIncreasingOrder: @escaping (Output, Output) throws -> Bool
    ) -> Publishers.TryComparison<Self> {
        return tryMax(by: { try areInIncreasingOrder($1, $0) })
    }

    public func max(
        by areInIncreasingOrder: @escaping (Output, Output) -> Bool
    ) -> Publishers.Comparison<Self> {
        return .init(upstream: self, areInIncreasingOrder: areInIncreasingOrder)
    }

    public func tryMax(
        by areInIncreasingOrder:  @escaping (Output, Output) throws -> Bool
    ) -> Publishers.TryComparison<Self> {
        return .init(upstream: self, areInIncreasingOrder: areInIncreasingOrder)
    }
}

extension Publishers {

    public struct Comparison<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let upstream: Upstream

        public let areInIncreasingOrder: (Upstream.Output, Upstream.Output) -> Bool

        public init(
            upstream: Upstream,
            areInIncreasingOrder: @escaping (Upstream.Output, Upstream.Output) -> Bool
        ) {
            self.upstream = upstream
            self.areInIncreasingOrder = areInIncreasingOrder
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Failure == Downstream.Failure,
                  Upstream.Output == Downstream.Input
        {
            let inner = Inner(downstream: subscriber,
                              areInIncreasingOrder: areInIncreasingOrder)
            upstream.subscribe(inner)
        }
    }

    public struct TryComparison<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Error

        public let upstream: Upstream

        public let areInIncreasingOrder: (Upstream.Output, Upstream.Output) throws -> Bool

        public init(
            upstream: Upstream,
            areInIncreasingOrder:
                @escaping (Upstream.Output, Upstream.Output) throws -> Bool
        ) {
            self.upstream = upstream
            self.areInIncreasingOrder = areInIncreasingOrder
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Upstream.Output == Downstream.Input, Downstream.Failure == Error
        {
            let inner = Inner(downstream: subscriber,
                              areInIncreasingOrder: areInIncreasingOrder)
            upstream.subscribe(inner)
        }
    }
}

extension Publishers.Comparison {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Upstream.Output, Upstream.Output) -> Bool>
    where Downstream.Input == Upstream.Output, Downstream.Failure == Upstream.Failure
    {
        fileprivate init(
            downstream: Downstream,
            areInIncreasingOrder: @escaping (Upstream.Output, Upstream.Output) -> Bool
        ) {
            super.init(downstream: downstream, initial: nil, reduce: areInIncreasingOrder)
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            if let result = self.result {
                if reduce(result, newValue) {
                    self.result = newValue
                }
            } else {
                self.result = newValue
            }
            return .continue
        }

        override var description: String {
            return "Comparison"
        }
    }
}

extension Publishers.TryComparison {
    private final class Inner<Downstream: Subscriber>
        : ReduceProducer<Downstream,
                         Upstream.Output,
                         Upstream.Output,
                         Upstream.Failure,
                         (Upstream.Output, Upstream.Output) throws -> Bool>
    where Downstream.Input == Upstream.Output, Downstream.Failure == Error
    {
        fileprivate init(
            downstream: Downstream,
            areInIncreasingOrder:
                @escaping (Upstream.Output, Upstream.Output) throws -> Bool
        ) {
            super.init(downstream: downstream, initial: nil, reduce: areInIncreasingOrder)
        }

        override func receive(
            newValue: Upstream.Output
        ) -> PartialCompletion<Void, Downstream.Failure> {
            do {
                if let result = self.result {
                    if try reduce(result, newValue) {
                        self.result = newValue
                    }
                } else {
                    self.result = newValue
                }
                return .continue
            } catch {
                return .failure(error)
            }
        }

        override var description: String {
            return "TryComparison"
        }
    }
}
