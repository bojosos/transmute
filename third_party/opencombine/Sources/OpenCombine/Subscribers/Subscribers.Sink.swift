//
//  Subscribers.Sink.swift
//  
//
//  Created by Sergej Jaskiewicz on 16.06.2019.
//

extension Subscribers {

    public final class Sink<Input, Failure: Error>
        : Subscriber,
          Cancellable,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
    {

        public var receiveValue: (Input) -> Void

        public var receiveCompletion: (Subscribers.Completion<Failure>) -> Void

        private var status = SubscriptionStatus.awaitingSubscription

        private let lock = UnfairLock.allocate()

        public var description: String { return "Sink" }

        public var customMirror: Mirror {
            return Mirror(self, children: EmptyCollection())
        }

        public var playgroundDescription: Any { return description }

        public init(
            receiveCompletion: @escaping (Subscribers.Completion<Failure>) -> Void,
            receiveValue: @escaping ((Input) -> Void)
        ) {
            self.receiveCompletion = receiveCompletion
            self.receiveValue = receiveValue
        }

        deinit {
            lock.deallocate()
        }

        public func receive(subscription: Subscription) {
            lock.lock()
            guard case .awaitingSubscription = status else {
                lock.unlock()
                subscription.cancel()
                return
            }
            status = .subscribed(subscription)
            lock.unlock()
            subscription.request(.unlimited)
        }

        public func receive(_ value: Input) -> Subscribers.Demand {
            lock.lock()
            let receiveValue = self.receiveValue
            lock.unlock()
            receiveValue(value)
            return .none
        }

        public func receive(completion: Subscribers.Completion<Failure>) {
            lock.lock()
            status = .terminal
            let receiveCompletion = self.receiveCompletion
            self.receiveCompletion = { _ in }

            // We MUST release the closures AFTER unlocking the lock,
            // since releasing a closure may trigger execution of arbitrary code,
            // for example, if the closure captures an object with a deinit.
            // When closure deallocates, the object's deinit is called, and holding
            // the lock at that moment can lead to deadlocks.
            // See https://github.com/OpenCombine/OpenCombine/issues/208

            withExtendedLifetime(receiveValue) {
                receiveValue = { _ in }
                lock.unlock()
            }

            receiveCompletion(completion)
        }

        public func cancel() {
            lock.lock()
            guard case let .subscribed(subscription) = status else {
                lock.unlock()
                return
            }
            status = .terminal

            // We MUST release the closures AFTER unlocking the lock,
            // since releasing a closure may trigger execution of arbitrary code,
            // for example, if the closure captures an object with a deinit.
            // When closure deallocates, the object's deinit is called, and holding
            // the lock at that moment can lead to deadlocks.
            // See https://github.com/OpenCombine/OpenCombine/issues/208

            withExtendedLifetime((receiveValue, receiveCompletion)) {
                receiveCompletion = { _ in }
                receiveValue = { _ in }
                lock.unlock()
            }
            subscription.cancel()
        }
    }
}

extension Publisher {

    public func sink(
        receiveCompletion: @escaping (Subscribers.Completion<Failure>) -> Void,
        receiveValue: @escaping ((Output) -> Void)
    ) -> AnyCancellable {
        let subscriber = Subscribers.Sink<Output, Failure>(
            receiveCompletion: receiveCompletion,
            receiveValue: receiveValue
        )
        subscribe(subscriber)
        return AnyCancellable(subscriber)
    }
}

extension Publisher where Failure == Never {

    public func sink(
        receiveValue: @escaping (Output) -> Void
    ) -> AnyCancellable {
        let subscriber = Subscribers.Sink<Output, Failure>(
            receiveCompletion: { _ in },
            receiveValue: receiveValue
        )
        subscribe(subscriber)
        return AnyCancellable(subscriber)
    }
}
