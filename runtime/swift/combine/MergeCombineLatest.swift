// Publishers.Merge, Merge3, Merge4 and CombineLatest, and Publisher.combineLatest(_:), which OpenCombine does not
// provide. Written for Alchemy from Combine's published API: the declarations follow the documented signatures; the
// implementation is our own, over Publishers.MergeMany (MergeMany.swift).
//
// Merge delivers the elements of its upstreams in arrival order, finishes once every upstream finished and fails with
// the first failure. CombineLatest delivers a tuple of every upstream's latest element each time any of them produces
// one, once each has produced at least one; it finishes once every upstream finished and fails with the first failure.

extension Publishers {

    public struct Merge<A: Publisher, B: Publisher>: Publisher
        where A.Failure == B.Failure, A.Output == B.Output
    {
        public typealias Output = A.Output
        public typealias Failure = A.Failure

        public let a: A
        public let b: B

        public init(_ a: A, _ b: B) {
            self.a = a
            self.b = b
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Failure, Downstream.Input == Output
        {
            Publishers.MergeMany(a.eraseToAnyPublisher(), b.eraseToAnyPublisher()).receive(subscriber: subscriber)
        }
    }

    public struct Merge3<A: Publisher, B: Publisher, C: Publisher>: Publisher
        where A.Failure == B.Failure, A.Output == B.Output, B.Failure == C.Failure, B.Output == C.Output
    {
        public typealias Output = A.Output
        public typealias Failure = A.Failure

        public let a: A
        public let b: B
        public let c: C

        public init(_ a: A, _ b: B, _ c: C) {
            self.a = a
            self.b = b
            self.c = c
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Failure, Downstream.Input == Output
        {
            Publishers.MergeMany(a.eraseToAnyPublisher(), b.eraseToAnyPublisher(), c.eraseToAnyPublisher())
                .receive(subscriber: subscriber)
        }
    }

    public struct Merge4<A: Publisher, B: Publisher, C: Publisher, D: Publisher>: Publisher
        where A.Failure == B.Failure, A.Output == B.Output, B.Failure == C.Failure, B.Output == C.Output,
              C.Failure == D.Failure, C.Output == D.Output
    {
        public typealias Output = A.Output
        public typealias Failure = A.Failure

        public let a: A
        public let b: B
        public let c: C
        public let d: D

        public init(_ a: A, _ b: B, _ c: C, _ d: D) {
            self.a = a
            self.b = b
            self.c = c
            self.d = d
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Failure, Downstream.Input == Output
        {
            Publishers.MergeMany(a.eraseToAnyPublisher(), b.eraseToAnyPublisher(), c.eraseToAnyPublisher(),
                                 d.eraseToAnyPublisher())
                .receive(subscriber: subscriber)
        }
    }

    public struct CombineLatest<A: Publisher, B: Publisher>: Publisher where A.Failure == B.Failure {
        public typealias Output = (A.Output, B.Output)
        public typealias Failure = A.Failure

        public let a: A
        public let b: B

        public init(_ a: A, _ b: B) {
            self.a = a
            self.b = b
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Failure == Failure, Downstream.Input == Output
        {
            typealias Latest = _CombineLatestValues<A.Output, B.Output>
            Publishers.MergeMany(a.map { Latest.Side.a($0) }.eraseToAnyPublisher(),
                                 b.map { Latest.Side.b($0) }.eraseToAnyPublisher())
                .scan(Latest()) { latest, side in latest.updated(side) }
                .compactMap { $0.both }
                .receive(subscriber: subscriber)
        }
    }
}

/// The latest element of each side of a CombineLatest.
internal struct _CombineLatestValues<First, Second> {
    enum Side {
        case a(First)
        case b(Second)
    }
    var first: First?
    var second: Second?
    func updated(_ side: Side) -> _CombineLatestValues {
        var result = self
        switch side {
        case .a(let value): result.first = value
        case .b(let value): result.second = value
        }
        return result
    }
    var both: (First, Second)? {
        guard let first, let second else { return nil }
        return (first, second)
    }
}

extension Publisher {

    public func combineLatest<P: Publisher>(_ other: P) -> Publishers.CombineLatest<Self, P> where Failure == P.Failure {
        Publishers.CombineLatest(self, other)
    }

    public func merge<P: Publisher>(with other: P) -> Publishers.Merge<Self, P> where Failure == P.Failure, Output == P.Output {
        Publishers.Merge(self, other)
    }

    public func merge<B: Publisher, C: Publisher>(with b: B, _ c: C) -> Publishers.Merge3<Self, B, C>
        where Failure == B.Failure, Output == B.Output, B.Failure == C.Failure, B.Output == C.Output
    {
        Publishers.Merge3(self, b, c)
    }
}
