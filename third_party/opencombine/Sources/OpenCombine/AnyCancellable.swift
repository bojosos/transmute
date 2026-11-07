//
//  AnyCancellable.swift
//
//
//  Created by Sergej Jaskiewicz on 26.06.2019.
//

public final class AnyCancellable: Cancellable, Hashable {

    private var _cancel: (() -> Void)?

    public init(_ cancel: @escaping () -> Void) {
        _cancel = cancel
    }

    public init<OtherCancellable: Cancellable>(_ canceller: OtherCancellable) {
        _cancel = canceller.cancel
    }

    public func cancel() {
        _cancel?()
        _cancel = nil
    }

    public static func == (lhs: AnyCancellable, rhs: AnyCancellable) -> Bool {
        return ObjectIdentifier(lhs) == ObjectIdentifier(rhs)
    }

    public func hash(into hasher: inout Hasher) {
        hasher.combine(ObjectIdentifier(self))
    }

    deinit {
        _cancel?()
    }
}

extension AnyCancellable {

    public func store<Cancellables: RangeReplaceableCollection>(
        in collection: inout Cancellables
    ) where Cancellables.Element == AnyCancellable {
        collection.append(self)
    }

    public func store(in set: inout Set<AnyCancellable>) {
        set.insert(self)
    }
}
