//
//  Record.swift
//  
//
//  Created by Sergej Jaskiewicz on 12.11.2019.
//

public struct Record<Output, Failure: Error>: Publisher {

    public let recording: Recording

    public init(record: (inout Recording) -> Void) {
        var recording = Recording()
        record(&recording)
        self.init(recording: recording)
    }

    public init(recording: Recording) {
        self.recording = recording
    }

    public init(output: [Output], completion: Subscribers.Completion<Failure>) {
        self.init(recording: Recording(output: output, completion: completion))
    }

    public func receive<Downstream: Subscriber>(subscriber: Downstream)
        where Output == Downstream.Input, Failure == Downstream.Failure
    {
        if recording.output.isEmpty {
            subscriber.receive(subscription: Subscriptions.empty)
            subscriber.receive(completion: recording.completion)
        } else {
            let inner = Inner(downstream: subscriber,
                              sequence: recording.output,
                              completion: recording.completion)
            subscriber.receive(subscription: inner)
        }
    }

    public struct Recording {

        public typealias Input = Output

        private enum State {
            case input
            case complete
        }

        private var state: State

        public private(set) var output: [Output]

        public private(set) var completion: Subscribers.Completion<Failure>

        public init() {
            state = .input
            output = []
            completion = .finished
        }

        public init(output: [Output],
                    completion: Subscribers.Completion<Failure> = .finished) {
            self.state = .complete
            self.output = output
            self.completion = completion
        }

        public mutating func receive(_ input: Input) {
            precondition(state == .input,
                         "Receiving values after completion is not allowed")
            output.append(input)
        }

        public mutating func receive(completion: Subscribers.Completion<Failure>) {
            precondition(state == .input,
                         "Receiving completion more than once is not allowed")
            self.completion = completion
            self.state = .complete
        }
    }
}

extension Record: Codable where Output: Codable, Failure: Codable {}

extension Record.Recording: Codable where Output: Codable, Failure: Codable {

    private enum CodingKeys: String, CodingKey {
        case output = "output"
        case completion = "completion"
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let output = try container.decode([Output].self, forKey: .output)
        let completion = try container.decode(Subscribers.Completion<Failure>.self,
                                              forKey: .completion)
        self.init(output: output, completion: completion)
    }

    public func encode(into encoder: Encoder) throws {
        try encode(to: encoder)
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(output, forKey: .output)
        try container.encode(completion, forKey: .completion)
    }
}

extension Record {

    // This class is almost the same as Publishers.Sequence.Inner
    // despite some small details.
    private final class Inner<Downstream: Subscriber>
        : Subscription,
          CustomStringConvertible,
          CustomReflectable,
          CustomPlaygroundDisplayConvertible
    where Downstream.Input == Output, Downstream.Failure == Failure
    {
        // NOTE: This class has been audited for thread-safety

        private var sequence: [Output]?
        private let completion: Subscribers.Completion<Failure>
        private var downstream: Downstream?
        private var iterator: IndexingIterator<[Output]>
        private var next: Output?
        private var pendingDemand = Subscribers.Demand.none
        private var recursion = false
        private var lock = UnfairLock.allocate()

        fileprivate init(downstream: Downstream,
                         sequence: [Output],
                         completion: Subscribers.Completion<Failure>) {
            self.sequence = sequence
            self.completion = completion
            self.downstream = downstream
            self.iterator = sequence.makeIterator()
            next = iterator.next()
        }

        deinit {
            lock.deallocate()
        }

        var description: String {
            lock.lock()
            defer { lock.unlock() }
            return sequence.map { $0.description } ?? "Cancelled Events"
        }

        var customMirror: Mirror {
            lock.lock()
            defer { lock.unlock() }
            let children: [Mirror.Child] = [
                ("sequence", sequence ?? [Output]()),
                ("completion", completion)
            ]
            return Mirror(self, children: children)
        }

        var playgroundDescription: Any { return description }

        func request(_ demand: Subscribers.Demand) {
            lock.lock()
            guard downstream != nil else {
                lock.unlock()
                return
            }
            pendingDemand += demand
            if recursion {
                lock.unlock()
                return
            }

            while let downstream = self.downstream, pendingDemand > 0 {
                if let current = self.next {
                    pendingDemand -= 1
                    let next = iterator.next()
                    recursion = true
                    lock.unlock()
                    let additionalDemand = downstream.receive(current)
                    lock.lock()
                    recursion = false
                    pendingDemand += additionalDemand
                    self.next = next
                }

                if next == nil {
                    self.downstream = nil
                    self.sequence = nil
                    lock.unlock()
                    downstream.receive(completion: completion)
                    return
                }
            }

            lock.unlock()
        }

        func cancel() {
            lock.lock()
            downstream = nil
            sequence = nil
            lock.unlock()
        }
    }
}
