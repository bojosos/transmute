//
//  Empty.swift
//  
//
//  Created by Sergej Jaskiewicz on 16.06.2019.
//

public struct Empty<Output, Failure: Error>: Publisher, Equatable {

    public init(completeImmediately: Bool = true) {
        self.completeImmediately = completeImmediately
    }

    public init(completeImmediately: Bool = true,
                outputType: Output.Type,
                failureType: Failure.Type) {
        self.init(completeImmediately: completeImmediately)
    }

    public let completeImmediately: Bool

    public func receive<Downstream: Subscriber>(subscriber: Downstream)
        where Output == Downstream.Input, Failure == Downstream.Failure
    {
        subscriber.receive(subscription: Subscriptions.empty)
        if completeImmediately {
            subscriber.receive(completion: .finished)
        }
    }
}
