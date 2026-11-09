// Project-authored Swift workload for libswiftFoundation's value types and
// libswiftObjectiveC: Notification bridging, NotificationCenter's and Timer's
// publishers, CharacterSet and the String conveniences, UUID, URL and
// URLComponents, IndexSet, JSON coding, key-value observing, Data's files,
// NSNumber bridging, os_log, Selector and NSObject hashing.

import Combine
import Foundation
import os

final class Thermostat: NSObject {
    @objc dynamic var temperature = 20

    override init() { super.init() }
}

struct Settings: Codable, Equatable {
    var name: String
    var level: Int
    var ratio: Double
    var tags: [String]
    var id: UUID
    var stamp: Date
    var blob: Data
    var note: String?
}

@main
struct SwiftFoundation {
    static func main() {
        let name = NSNotificationName(rawValue: "AlchemyPing"._bridgeToObjectiveC())
        let note = Notification(name: name, object: nil, userInfo: ["count": 3])
        let back = Notification._unconditionallyBridgeFromObjectiveC(note._bridgeToObjectiveC())
        print("notification", String._unconditionallyBridgeFromObjectiveC(back.name.rawValue), back.userInfo?["count"] as? Int ?? -1,
              back == note)

        let center = NSNotificationCenter.default
        var received = [String]()
        let subscription = center.publisher(for: name).sink { received.append(String._unconditionallyBridgeFromObjectiveC($0.name.rawValue)) }
        center.post(note)
        center.post(Notification(name: name))
        subscription.cancel()
        center.post(note)
        print("publisher", received)

        var ticks = 0
        let timer = NSTimer.publish(every: 0.05, on: NSRunLoop.current, in: .defaultRunLoopMode).autoconnect().sink { _ in ticks += 1 }
        NSRunLoop.current.run(until: Date(timeIntervalSinceNow: 0.4)._bridgeToObjectiveC())
        timer.cancel()
        print("timer", ticks >= 2)

        print("trim", "[\("  hello world \n".trimmingCharacters(in: .whitespacesAndNewlines))]")
        print("split", "a, b;c".components(separatedBy: CharacterSet(charactersIn: ",; ")).filter { !$0.isEmpty })
        print("percent", "a b&c".addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? "nil")
        print("format", String(format: "%d-%@-%.2f", 7, "x", 1.5))
        print("case", "hello world".capitalized, "abc".caseInsensitiveCompare("ABC") == .orderedSame)
        print("replace", "a-b-c".replacingOccurrences(of: "-", with: "+"))
        let text = "h\u{E9}llo"
        let range = NSRange(text.index(after: text.startIndex) ..< text.endIndex, in: text)
        print("range", range.location, range.length, Range(NSRange(location: 1, length: 2), in: text).map { String(text[$0]) } ?? "nil")

        let uuid = UUID(uuidString: "e621e1f8-c36c-495a-93fc-0c247a3e6e5f")!
        let fresh = UUID()
        print("uuid", uuid.uuidString, fresh != UUID(), fresh.uuidString.count, UUID(uuidString: "nope") == nil)

        let url = URL(string: "https://example.com/a/b?x=1&y=two#frag")!
        print("url", url.scheme ?? "", url.host ?? "", url.pathComponents, url.fragment ?? "")
        let components = URLComponents(url: url, resolvingAgainstBaseURL: false)!
        print("query", components.queryItems?.map { "\($0.name)=\($0.value ?? "")" } ?? [])

        var indexes = IndexSet(integersIn: 1 ..< 4)
        indexes.insert(7)
        indexes.remove(2)
        print("indexset", Array(indexes), indexes.count, indexes.contains(3))

        let settings = Settings(name: "pika \"one\"", level: 3, ratio: 0.25, tags: ["a", "b/c"], id: uuid,
                                stamp: Date(timeIntervalSinceReferenceDate: 86400), blob: Data([1, 2, 255]), note: nil)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let json = try! encoder.encode(settings)
        print("json", String(data: json, encoding: .utf8)!)
        print("roundtrip", (try? JSONDecoder().decode(Settings.self, from: json)) == settings)
        do {
            _ = try JSONDecoder().decode(Settings.self, from: Data("{\"name\": 1}".utf8))
        } catch let DecodingError.typeMismatch(type, _) {
            print("mismatch", type)
        } catch {
            print("unexpected", error)
        }

        let thermostat = Thermostat()
        var changes = [String]()
        let observation = thermostat.observe(\.temperature, options: [.old, .new]) { _, change in
            changes.append("\(change.oldValue ?? -1)->\(change.newValue ?? -1)")
        }
        // The change notifications come from willChangeValue/didChangeValue(for:), the path compiled apps use
        // around their own setters.
        for value in [25, 30] {
            thermostat.willChangeValue(for: \.temperature)
            thermostat.temperature = value
            thermostat.didChangeValue(for: \.temperature)
        }
        observation.invalidate()
        thermostat.willChangeValue(for: \.temperature)
        thermostat.temperature = 1
        thermostat.didChangeValue(for: \.temperature)
        print("kvo", changes)

        let file = URL(fileURLWithPath: "/tmp/alchemy-swift-foundation.bin")
        try! Data([1, 2, 3]).write(to: file)
        print("data", Array(try! Data(contentsOf: file)))

        print("number", 42._bridgeToObjectiveC().integerValue, true._bridgeToObjectiveC().boolValue, 1.5._bridgeToObjectiveC().doubleValue)

        os_log(.default, "swift_foundation %d %@", 1, "logged")
        os_log("swift_foundation default")

        let object: NSObject = NSObject()
        print("objc", Selector(("description")), Selector(("a:")) == Selector(("a:")), Set([object, object]).count, object == object)
        print("done")
    }
}
