//
//  AnyPublisher.swift
//  
//
//  Created by Sergej Jaskiewicz on 10.06.2019.
//

extension Publisher {

    public func eraseToAnyPublisher() -> AnyPublisher<Output, Failure> {
        return .init(self)
    }
}

public struct AnyPublisher<Output, Failure: Error>
  : CustomStringConvertible,
    CustomPlaygroundDisplayConvertible
{
    @usableFromInline
    internal let box: PublisherBoxBase<Output, Failure>

    public init<PublisherType: Publisher>(_ publisher: PublisherType)
        where Output == PublisherType.Output, Failure == PublisherType.Failure
    {
        // If this has already been boxed, avoid boxing again
        if let erased = publisher as? AnyPublisher<Output, Failure> {
            box = erased.box
        } else {
            box = PublisherBox(base: publisher)
        }
    }

    public var description: String {
        return "AnyPublisher"
    }

    public var playgroundDescription: Any {
        return description
    }
}

extension AnyPublisher: Publisher {

    public func receive<Downstream: Subscriber>(subscriber: Downstream)
        where Output == Downstream.Input, Failure == Downstream.Failure
    {
        box.receive(subscriber: subscriber)
    }
}

@usableFromInline
internal class PublisherBoxBase<Output, Failure: Error>: Publisher {

    internal init() {}

    @usableFromInline
    internal func receive<Downstream: Subscriber>(subscriber: Downstream)
        where Failure == Downstream.Failure, Output == Downstream.Input
    {
        abstractMethod()
    }
}

@usableFromInline
internal final class PublisherBox<PublisherType: Publisher>
    : PublisherBoxBase<PublisherType.Output, PublisherType.Failure>
{
    @usableFromInline
    internal let base: PublisherType

    internal init(base: PublisherType) {
        self.base = base
        super.init()
    }

    override internal func receive<Downstream: Subscriber>(subscriber: Downstream)
        where Failure == Downstream.Failure, Output == Downstream.Input
    {
        base.receive(subscriber: subscriber)
    }
}
