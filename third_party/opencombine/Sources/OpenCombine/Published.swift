//
//  Published.swift
//  OpenCombine
//
//  Created by Евгений Богомолов on 01/09/2019.
//

#if swift(>=5.1)

extension Publisher where Failure == Never {

    public func assign(to published: inout Published<Output>.Publisher) {
        subscribe(PublishedSubscriber(published.subject))
    }
}

@available(swift, introduced: 5.1)
@propertyWrapper
public struct Published<Value> {

    public struct Publisher: Combine.Publisher {

        public typealias Output = Value

        public typealias Failure = Never

        fileprivate let subject: PublishedSubject<Value>

        public func receive<Downstream: Subscriber>(subscriber: Downstream)
            where Downstream.Input == Value, Downstream.Failure == Never
        {
            subject.subscribe(subscriber)
        }

        fileprivate init(_ output: Output) {
            subject = .init(output)
        }
    }

    private enum Storage {
        case value(Value)
        case publisher(Publisher)
    }
    @propertyWrapper
    private final class Box {
        var wrappedValue: Storage

        init(wrappedValue: Storage) {
            self.wrappedValue = wrappedValue
        }
    }

    @Box private var storage: Storage

    internal var objectWillChange: ObservableObjectPublisher? {
        get {
            switch storage {
            case .value:
                return nil
            case .publisher(let publisher):
                return publisher.subject.objectWillChange
            }
        }
        nonmutating set {
            getPublisher().subject.objectWillChange = newValue
        }
    }

    public init(initialValue: Value) {
        self.init(wrappedValue: initialValue)
    }

    public init(wrappedValue: Value) {
        _storage = Box(wrappedValue: .value(wrappedValue))
    }

    public var projectedValue: Publisher {
        mutating get {
            return getPublisher()
        }
        set { // swiftlint:disable:this unused_setter_value
            switch storage {
            case .value(let value):
                storage = .publisher(Publisher(value))
            case .publisher:
                break
            }
        }
    }

    internal func getPublisher() -> Publisher {
        switch storage {
        case .value(let value):
            let publisher = Publisher(value)
            storage = .publisher(publisher)
            return publisher
        case .publisher(let publisher):
            return publisher
        }
    }
    // swiftlint:disable let_var_whitespace
    @available(*, unavailable, message: """
               @Published is only available on properties of classes
               """)
    public var wrappedValue: Value {
        get { fatalError() }
        set { fatalError() } // swiftlint:disable:this unused_setter_value
    }
    // swiftlint:enable let_var_whitespace

    public static subscript<EnclosingSelf: AnyObject>(
        _enclosingInstance object: EnclosingSelf,
        wrapped wrappedKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Value>,
        storage storageKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Published<Value>>
    ) -> Value {
        get {
            switch object[keyPath: storageKeyPath].storage {
            case .value(let value):
                return value
            case .publisher(let publisher):
                return publisher.subject.value
            }
        }
        set {
            switch object[keyPath: storageKeyPath].storage {
            case .value:
                object[keyPath: storageKeyPath].storage = .publisher(Publisher(newValue))
            case .publisher(let publisher):
                publisher.subject.value = newValue
            }
        }
        // TODO: Benchmark and explore a possibility to use _modify
    }
}
#else

@available(swift, introduced: 5.1)
public typealias Published = Never

#endif // swift(>=5.1)
