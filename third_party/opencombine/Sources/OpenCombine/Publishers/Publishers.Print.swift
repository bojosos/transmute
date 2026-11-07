//
//  Publishers.Print.swift
//  
//
//  Created by Sergej Jaskiewicz on 16.06.2019.
//

extension Publisher {

    public func print(_ prefix: String = "",
                      to stream: TextOutputStream? = nil) -> Publishers.Print<Self> {
        return .init(upstream: self, prefix: prefix, to: stream)
    }
}

extension Publishers {

    public struct Print<Upstream: Publisher>: Publisher {

        public typealias Output = Upstream.Output

        public typealias Failure = Upstream.Failure

        public let prefix: String

        public let upstream: Upstream

        public let stream: TextOutputStream?

        public init(upstream: Upstream,
                    prefix: String,
                    to stream: TextOutputStream? = nil) {
            self.upstream = upstream
            self.prefix = prefix
            self.stream = stream
        }

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Failure == Downstream.Failure, Output == Downstream.Input
        {
            let inner = Inner(downstream: subscriber, prefix: prefix, stream: stream)
            upstream.subscribe(inner)
        }
    }
}

extension Publishers.Print {
    private final class Inner<Downstream: Subscriber>: Subscriber,
                                                       Subscription,
                                                       CustomStringConvertible,
                                                       CustomReflectable,
                                                       CustomPlaygroundDisplayConvertible
    {
        typealias Input = Downstream.Input
        typealias Failure = Downstream.Failure

        private struct PrintTarget: TextOutputStream {

            var stream: TextOutputStream

            mutating func write(_ string: String) {
                stream.write(string)
            }
        }

        private var downstream: Downstream
        private let prefix: String
        private var stream: PrintTarget?
        private var status = SubscriptionStatus.awaitingSubscription
        private let lock = UnfairLock.allocate()

        init(downstream: Downstream, prefix: String, stream: TextOutputStream?) {
            self.downstream = downstream
            self.prefix = prefix.isEmpty ? "" : "\(prefix): "
            self.stream = stream.map(PrintTarget.init)
        }

        deinit {
            lock.deallocate()
        }

        func receive(subscription: Subscription) {
            log("\(prefix)receive subscription: (\(subscription))")
            lock.lock()
            guard case .awaitingSubscription = status else {
                lock.unlock()
                subscription.cancel()
                return
            }
            status = .subscribed(subscription)
            lock.unlock()
            downstream.receive(subscription: self)
        }

        func receive(_ input: Input) -> Subscribers.Demand {
            log("\(prefix)receive value: (\(input))")
            let demand = downstream.receive(input)

            if let max = demand.max {
                log("\(prefix)request max: (\(max)) (synchronous)")
            } else {
                log("\(prefix)request unlimited (synchronous)")
            }

            return demand
        }

        func receive(completion: Subscribers.Completion<Failure>) {
            switch completion {
            case .finished:
                log("\(prefix)receive finished")
            case .failure(let error):
                log("\(prefix)receive error: (\(error))")
            }
            lock.lock()
            status = .terminal
            lock.unlock()
            downstream.receive(completion: completion)
        }

        func request(_ demand: Subscribers.Demand) {
            if let max = demand.max {
                log("\(prefix)request max: (\(max))")
            } else {
                log("\(prefix)request unlimited")
            }
            lock.lock()
            guard case let .subscribed(subscription) = status else {
                lock.unlock()
                return
            }
            lock.unlock()
            subscription.request(demand)
        }

        func cancel() {
            log("\(prefix)receive cancel")
            lock.lock()
            guard case let .subscribed(subscription) = status else {
                lock.unlock()
                return
            }
            status = .terminal
            lock.unlock()
            subscription.cancel()
        }

        var description: String { return "Print" }

        var customMirror: Mirror { return Mirror(self, children: EmptyCollection()) }

        var playgroundDescription: Any { return description }

        // MARK: - Private

        private func log(_ text: String) {
            if var stream = stream {
                Swift.print(text, to: &stream)
            } else {
                Swift.print(text)
            }
        }
    }
}
