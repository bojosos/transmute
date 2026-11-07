//
//  Subscriber+Void.swift
//  OpenCombine
//
//  Created by Sergej Jaskiewicz on 10.06.2019.
//

extension Subscriber where Input == Void {

    public func receive() -> Subscribers.Demand {
        return receive(())
    }
}
