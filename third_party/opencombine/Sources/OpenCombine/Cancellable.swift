//
//  Cancellable.swift
//  
//
//  Created by Sergej Jaskiewicz on 10.06.2019.
//

public protocol Cancellable {

    func cancel()
}

extension Cancellable {

    public func store<Cancellables: RangeReplaceableCollection>(
            in collection: inout Cancellables
    ) where Cancellables.Element == AnyCancellable {
        AnyCancellable(self).store(in: &collection)
    }

    public func store(in set: inout Set<AnyCancellable>) {
        AnyCancellable(self).store(in: &set)
    }
}
