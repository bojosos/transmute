//
//  Deferred.swift
//  
//
//  Created by Joseph Spadafora on 7/7/19.
//

public struct Deferred<DeferredPublisher: Publisher>: Publisher {

    public typealias Output = DeferredPublisher.Output

    public typealias Failure = DeferredPublisher.Failure

    public let createPublisher: () -> DeferredPublisher

    public init(createPublisher: @escaping () -> DeferredPublisher) {
        self.createPublisher = createPublisher
    }

    public func receive<Downstream: Subscriber>(subscriber: Downstream)
        where Failure == Downstream.Failure,
              Output == Downstream.Input
    {
        let deferredPublisher = createPublisher()
        deferredPublisher.subscribe(subscriber)
    }
}
