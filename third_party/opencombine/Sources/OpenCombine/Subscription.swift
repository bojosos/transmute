//
//  Subscription.swift
//  OpenCombine
//
//  Created by Sergej Jaskiewicz on 10.06.2019.
//

public protocol Subscription: Cancellable, CustomCombineIdentifierConvertible {

    func request(_ demand: Subscribers.Demand)
}
