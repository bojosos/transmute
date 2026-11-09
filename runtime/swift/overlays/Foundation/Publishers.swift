// Foundation's Combine publishers (project-authored, from Apple's public
// Foundation and Combine documentation): NotificationCenter.Publisher and
// Timer.TimerPublisher, with RunLoop.SchedulerOptions.
import Combine
import Darwin

// MARK: - NotificationCenter.Publisher

extension NSNotificationCenter {
    /// A publisher that emits each notification `center` posts with `name`
    /// (and `object`, when given). It never fails or finishes.
    public struct Publisher: Combine.Publisher {
        public typealias Output = Notification
        public typealias Failure = Never

        public let center: NSNotificationCenter
        public let name: NSNotificationName
        public let object: AnyObject?

        public init(center: NSNotificationCenter, name: NSNotificationName, object: AnyObject? = nil) {
            self.center = center
            self.name = name
            self.object = object
        }

        public func receive<S: Subscriber>(subscriber: S) where S.Input == Output, S.Failure == Failure {
            subscriber.receive(subscription: _NotificationSubscription(center: center, name: name, object: object,
                                                                        subscriber: subscriber))
        }
    }

    public func publisher(for name: NSNotificationName, object: AnyObject? = nil) -> NSNotificationCenter.Publisher {
        return Publisher(center: self, name: name, object: object)
    }
}

/// One subscriber's registration: notifications are delivered while demand
/// remains; cancelling removes the observer.
private final class _NotificationSubscription<S: Subscriber>: Subscription, @unchecked Sendable
    where S.Input == Notification, S.Failure == Never {
    private let lock = _SubscriptionLock()
    private var subscriber: S?
    private var demand = Subscribers.Demand.none
    private var observer: NSObjectProtocol?
    private let center: NSNotificationCenter

    init(center: NSNotificationCenter, name: NSNotificationName, object: AnyObject?, subscriber: S) {
        self.center = center
        self.subscriber = subscriber
        observer = center.addObserver(forName: name, object: object, queue: nil) { [weak self] notification in
            self?.deliver(Notification(_reference: notification))
        }
    }

    private func deliver(_ notification: Notification) {
        lock.lock()
        guard let subscriber = subscriber, demand > .none else { lock.unlock(); return }
        demand -= 1
        lock.unlock()
        let more = subscriber.receive(notification)
        lock.lock()
        demand += more
        lock.unlock()
    }

    func request(_ demand: Subscribers.Demand) {
        lock.lock()
        self.demand += demand
        lock.unlock()
    }

    func cancel() {
        lock.lock()
        let observer = self.observer
        self.observer = nil
        subscriber = nil
        lock.unlock()
        if let observer = observer { center.removeObserver(observer) }
    }
}

// MARK: - RunLoop.SchedulerOptions and Timer.TimerPublisher

extension NSRunLoop {
    /// Options for scheduling work on a run loop (none are defined).
    public struct SchedulerOptions {}
}

extension NSTimer {
    /// A publisher that emits the current date every `interval` seconds on
    /// `runLoop` in `mode`, once connected.
    public static func publish(every interval: TimeInterval, tolerance: TimeInterval? = nil, on runLoop: NSRunLoop,
                               in mode: NSRunLoopMode, options: NSRunLoop.SchedulerOptions? = nil) -> NSTimer.TimerPublisher {
        return TimerPublisher(interval: interval, tolerance: tolerance, runLoop: runLoop, mode: mode, options: options)
    }

    public final class TimerPublisher: ConnectablePublisher {
        public typealias Output = Date
        public typealias Failure = Never

        public let interval: TimeInterval
        public let tolerance: TimeInterval?
        public let runLoop: NSRunLoop
        public let mode: NSRunLoopMode
        public let options: NSRunLoop.SchedulerOptions?

        private let lock = _SubscriptionLock()
        private var subscriptions: [_TimerSubscription] = []
        private var timer: NSTimer?

        public init(interval: TimeInterval, tolerance: TimeInterval? = nil, runLoop: NSRunLoop, mode: NSRunLoopMode,
                    options: NSRunLoop.SchedulerOptions? = nil) {
            self.interval = interval
            self.tolerance = tolerance
            self.runLoop = runLoop
            self.mode = mode
            self.options = options
        }

        public func receive<S: Subscriber>(subscriber: S) where S.Input == Output, S.Failure == Failure {
            let subscription = _TimerSubscription(publisher: self, subscriber: AnySubscriber(subscriber))
            lock.lock()
            subscriptions.append(subscription)
            lock.unlock()
            subscriber.receive(subscription: subscription)
        }

        /// Starts the timer; cancelling the result stops it.
        public func connect() -> Cancellable {
            lock.lock()
            if timer == nil {
                let timer = NSTimer(timeInterval: interval, repeats: true) { [weak self] _ in self?.fire() }
                if let tolerance = tolerance { timer.tolerance = tolerance }
                self.timer = timer
                lock.unlock()
                runLoop.add(timer, forMode: mode)
            } else {
                lock.unlock()
            }
            return AnyCancellable { [weak self] in self?.disconnect() }
        }

        private func disconnect() {
            lock.lock()
            let timer = self.timer
            self.timer = nil
            lock.unlock()
            timer?.invalidate()
        }

        private func fire() {
            let now = Date()
            lock.lock()
            let current = subscriptions
            lock.unlock()
            for subscription in current { subscription.deliver(now) }
        }

        fileprivate func remove(_ subscription: _TimerSubscription) {
            lock.lock()
            subscriptions.removeAll { $0 === subscription }
            lock.unlock()
        }
    }
}

private final class _TimerSubscription: Subscription, @unchecked Sendable {
    private let lock = _SubscriptionLock()
    private weak var publisher: NSTimer.TimerPublisher?
    private var subscriber: AnySubscriber<Date, Never>?
    private var demand = Subscribers.Demand.none

    init(publisher: NSTimer.TimerPublisher, subscriber: AnySubscriber<Date, Never>) {
        self.publisher = publisher
        self.subscriber = subscriber
    }

    func deliver(_ date: Date) {
        lock.lock()
        guard let subscriber = subscriber, demand > .none else { lock.unlock(); return }
        demand -= 1
        lock.unlock()
        let more = subscriber.receive(date)
        lock.lock()
        demand += more
        lock.unlock()
    }

    func request(_ demand: Subscribers.Demand) {
        lock.lock()
        self.demand += demand
        lock.unlock()
    }

    func cancel() {
        lock.lock()
        subscriber = nil
        let publisher = self.publisher
        lock.unlock()
        publisher?.remove(self)
    }
}

/// The lock the subscriptions above use (an os_unfair_lock in its own
/// allocation, so its address is stable).
private final class _SubscriptionLock: @unchecked Sendable {
    private let pointer: UnsafeMutablePointer<os_unfair_lock>

    init() {
        pointer = UnsafeMutablePointer<os_unfair_lock>.allocate(capacity: 1)
        pointer.initialize(to: os_unfair_lock())
    }

    deinit {
        pointer.deinitialize(count: 1)
        pointer.deallocate()
    }

    func lock() { os_unfair_lock_lock(pointer) }
    func unlock() { os_unfair_lock_unlock(pointer) }
}
