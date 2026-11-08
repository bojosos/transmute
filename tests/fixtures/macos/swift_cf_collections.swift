// Project-authored Swift workload for CoreFoundation and Foundation meeting
// Swift's bridged collections (guest-implemented NSArray and NSDictionary
// objects): nested collections, the elements Swift bridges lazily, a retaining
// CFArray's balance, a collection used as a property list (registerDefaults
// and CFPropertyListCreateData), and an element that isn't a property-list
// object, which fails where CoreFoundation fails.

import Foundation

final class Marker: NSObject {
    override init() { super.init() }
}

func cf(_ object: AnyObject) -> UnsafeRawPointer { return UnsafeRawPointer(Unmanaged.passUnretained(object).toOpaque()) }

func string(_ pointer: UnsafeRawPointer?) -> String {
    guard let pointer = pointer else { return "nil" }
    let text = unsafeBitCast(pointer, to: CFString.self)
    let length = CFStringGetLength(text)
    var units = [UInt16](repeating: 0, count: length)
    CFStringGetCharacters(text, CFRange(location: 0, length: length), &units)
    return String(decoding: units, as: UTF16.self)
}

func ns(_ text: String) -> NSString { return text._bridgeToObjectiveC() }

@main
struct SwiftCFCollections {
    static func main() {
        // An array of dictionaries of strings.
        let rows: [[String: String]] = [["name": "first-row-name"], ["name": "second-row-name", "kind": "a longer kind value"]]
        let array = rows._bridgeToObjectiveC()
        let raw = unsafeBitCast(array, to: CFArray.self)
        let second = CFArrayGetValueAtIndex(raw, 1)!
        let dictionary = unsafeBitCast(second, to: CFDictionary.self)
        let kind = ns("kind")
        print("nested", CFArrayGetCount(raw), CFDictionaryGetCount(dictionary),
              withExtendedLifetime(kind) { string(CFDictionaryGetValue(dictionary, cf(kind))) })
        // Swift bridges each element once and keeps it with the collection.
        print("elements", CFArrayGetValueAtIndex(raw, 1) == second, CFArrayGetValueAtIndex(raw, 0) != second)

        // CF's retains and releases reach the collection's own reference count.
        let before = _getRetainCount(array)
        var callbacks = kCFTypeArrayCallBacks
        var retained = UInt(0)
        do {
            let holder = CFArrayCreateMutable(nil, 0, &callbacks)!.takeRetainedValue()
            CFArrayAppendValue(holder, cf(array))
            retained = _getRetainCount(array)
        }
        print("balance", retained == before + 1, _getRetainCount(array) == before)

        // A dictionary as a property list: registered defaults, and data CF writes and reads back.
        let defaults: [String: Any] = ["SwiftCFCollectionsLevel": 7, "SwiftCFCollectionsNames": ["one", "two"],
                                       "SwiftCFCollectionsName": "registered value"]
        let registered = defaults._bridgeToObjectiveC()
        NSUserDefaults.standard.register(registered)
        print("defaults", NSUserDefaults.standard.integer(forKey: ns("SwiftCFCollectionsLevel")),
              string(NSUserDefaults.standard.object(forKey: ns("SwiftCFCollectionsName")).map { cf($0 as AnyObject) }))
        let data = CFPropertyListCreateData(nil, registered, kCFPropertyListXMLFormat_v1_0, 0, nil)!.takeRetainedValue()
        let parsed = CFPropertyListCreateWithData(nil, data, 0, nil, nil)!.takeRetainedValue()
        print("plist", CFDataGetLength(data) > 0, CFDictionaryGetCount(unsafeBitCast(parsed, to: CFDictionary.self)))

        // An element that isn't a property-list object: CF answers NULL with an error, as it does on macOS.
        let mixed: [Any] = [ns("fine"), Marker()]
        let bridgedMixed = mixed._bridgeToObjectiveC()
        var error: Unmanaged<CFError>?
        let refused = CFPropertyListCreateData(nil, bridgedMixed, kCFPropertyListXMLFormat_v1_0, 0, &error)
        print("invalid", CFArrayGetCount(unsafeBitCast(bridgedMixed, to: CFArray.self)), refused == nil, error != nil)
        error?.release()
        print("done")
    }
}
