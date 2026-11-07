//
//  Fail.swift
//  
//
//  Created by Sergej Jaskiewicz on 19.06.2019.
//

public struct Fail<Output, Failure: Error>: Publisher {

    public init(error: Failure) {
        self.error = error
    }

    public init(outputType: Output.Type, failure: Failure) {
        self.error = failure
    }

    public let error: Failure

    public func receive<Downstream: Subscriber>(subscriber: Downstream)
        where Output == Downstream.Input, Failure == Downstream.Failure
    {
        subscriber.receive(subscription: Subscriptions.empty)
        subscriber.receive(completion: .failure(error))
    }
}

extension Fail: Equatable where Failure: Equatable {}
