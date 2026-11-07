//
//  Publishers.Share
//  
//
//  Created by Sergej Jaskiewicz on 18/09/2019.
//

extension Publisher {

    public func share() -> Publishers.Share<Self> {
        return .init(upstream: self)
    }
}

extension Publishers {

    public final class Share<Upstream: Publisher>: Publisher, Equatable {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        private typealias MulticastSubject = PassthroughSubject<Output, Failure>

        private let inner: Autoconnect<Multicast<Upstream, MulticastSubject>>

        public let upstream: Upstream

        public init(upstream: Upstream) {
            self.inner = upstream.multicast(subject: .init()).autoconnect()
            self.upstream = upstream
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Input == Output, Downstream.Failure == Failure
        {
            inner.subscribe(subscriber)
        }

        public static func == (lhs: Share, rhs: Share) -> Bool {
            return lhs === rhs
        }
    }
}
