// Project-authored Swift workload for CoreFoundation meeting Swift's bridged
// strings (guest-implemented NSString objects): CFStringGetLength and
// CFStringGetCharacters, NSNotification, a retaining CFArray's balance, a
// string Swift changes in place, an address a new string reuses, and a
// dictionary key whose String changes afterwards.

import Foundation

func cf(_ object: NSString) -> CFString { return unsafeBitCast(object, to: CFString.self) }

func characters(_ string: CFString) -> String {
    let length = CFStringGetLength(string)
    var units = [UInt16](repeating: 0, count: length)
    CFStringGetCharacters(string, CFRange(location: 0, length: length), &units)
    return String(decoding: units, as: UTF16.self)
}

func pointer(_ object: NSString) -> UnsafeRawPointer { return UnsafeRawPointer(Unmanaged.passUnretained(object).toOpaque()) }

@main
struct SwiftCFStrings {
    static func main() {
        // Longer than a small string, so it bridges as Swift's native string storage object.
        let text = String(repeating: "swift-", count: 5) + "\u{E9}"
        let bridged = text._bridgeToObjectiveC()
        print("length", CFStringGetLength(cf(bridged)), characters(cf(bridged)) == text)
        let note = NSNotification(name: NSNotificationName(rawValue: bridged), object: nil, userInfo: nil)
        print("notification", String._unconditionallyBridgeFromObjectiveC(note.name.rawValue) == text)

        // CF's retains and releases reach the string's own reference count.
        let before = _getRetainCount(bridged)
        var callbacks = kCFTypeArrayCallBacks
        var retained = UInt(0)
        do {
            let array = CFArrayCreateMutable(nil, 0, &callbacks)!.takeRetainedValue()
            CFArrayAppendValue(array, pointer(bridged))
            retained = _getRetainCount(bridged)
        }
        print("balance", retained == before + 1, _getRetainCount(bridged) == before)

        // Uniquely referenced again, the String may change its storage in place; a second bridge sees the change.
        var growing = String(repeating: "x", count: 40)
        do {
            let first = growing._bridgeToObjectiveC()
            print("before", CFStringGetLength(cf(first)))
        }
        growing.append("y")
        let second = growing._bridgeToObjectiveC()
        print("after", CFStringGetLength(cf(second)), characters(cf(second)) == growing)

        // A freed string's address taken by a new string: no stale characters.
        var reused = false, correct = true
        for _ in 0 ..< 64 where !reused {
            var address = 0
            do {
                let old = String(repeating: "a", count: 30)._bridgeToObjectiveC()
                address = Int(bitPattern: pointer(old))
                correct = correct && characters(cf(old)) == String(repeating: "a", count: 30)
            }
            let new = String(repeating: "b", count: 30)._bridgeToObjectiveC()
            reused = Int(bitPattern: pointer(new)) == address
            correct = correct && characters(cf(new)) == String(repeating: "b", count: 30)
        }
        print("reuse", reused, correct)

        // The dictionary retains its key, so changing the String afterwards copies it instead.
        var key = String(repeating: "k", count: 24)
        // CF recognizes the standard callbacks by their contents, so copies of the records serve.
        var keyCallBacks = kCFTypeDictionaryKeyCallBacks, valueCallBacks = kCFTypeDictionaryValueCallBacks
        let dictionary = CFDictionaryCreateMutable(nil, 0, &keyCallBacks, &valueCallBacks)!.takeRetainedValue()
        do {
            let bridgedKey = key._bridgeToObjectiveC(), value = "value"._bridgeToObjectiveC()
            CFDictionarySetValue(dictionary, pointer(bridgedKey), pointer(value))
        }
        key.append("!")
        let lookup = String(repeating: "k", count: 24)._bridgeToObjectiveC()
        let found = withExtendedLifetime(lookup) { CFDictionaryGetValue(dictionary, pointer(lookup)) }
        print("dictionary", CFDictionaryGetCount(dictionary), found != nil, key.count)
        print("done")
    }
}
