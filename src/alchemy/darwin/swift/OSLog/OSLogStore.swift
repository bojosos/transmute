// Alchemy's project-authored OSLog overlay (/usr/lib/swift/
// libswiftOSLog.dylib): OSLogStore's Swift entry enumeration, bounded by
// what QLab 5.6.3 uses, written from Apple's public documentation. The
// Objective-C enumerator's entries are delivered lazily in order.

import Foundation
@_exported import OSLog // The Clang module (Alchemy's OSLog declarations).

extension OSLogStore {
    /// Returns the log entries that match the given options, starting position and predicate.
    public func getEntries(with options: OSLogEnumerator.Options = [], at position: OSLogPosition? = nil,
                           matching predicate: NSPredicate? = nil) throws -> AnySequence<OSLogEntry> {
        let enumerator = try __entriesEnumerator(options: options, position: position, predicate: predicate)
        return AnySequence {
            AnyIterator { () -> OSLogEntry? in
                guard let next = enumerator.nextObject() else { return nil }
                return (next as! OSLogEntry)
            }
        }
    }
}
