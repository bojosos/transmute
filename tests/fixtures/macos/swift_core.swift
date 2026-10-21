// Project-authored Swift workload for the guest Swift runtime (libswiftCore):
// classes and inheritance, generics, protocols and existentials, dynamic
// casts, errors, closures, String, Array, Dictionary and Set.

protocol Shape {
    var name: String { get }
    func area() -> Int
}

final class Square: Shape {
    let side: Int
    init(side: Int) { self.side = side }
    var name: String { "square" }
    func area() -> Int { side * side }
}

struct Rectangle: Shape {
    var width: Int, height: Int
    var name: String { "rectangle" }
    func area() -> Int { width * height }
}

class Animal {
    let label: String
    init(label: String) { self.label = label }
    func sound() -> String { "..." }
    deinit { Animal.released += 1 }
    static var released = 0
}

final class Dog: Animal {
    override func sound() -> String { "woof" }
}

enum Failure: Error, Equatable {
    case negative(Int)
    case tooLarge
}

func doubled(_ value: Int) throws -> Int {
    if value < 0 { throw Failure.negative(value) }
    if value > 1000 { throw Failure.tooLarge }
    return value * 2
}

func totalArea<S: Shape>(_ shapes: [S]) -> Int {
    shapes.reduce(0) { $0 + $1.area() }
}

struct Box<T> {
    var value: T
    func map<U>(_ transform: (T) -> U) -> Box<U> { Box<U>(value: transform(value)) }
}

var lines: [String] = []

// Protocols, existentials and generics.
let shapes: [any Shape] = [Square(side: 3), Rectangle(width: 2, height: 5)]
let names = shapes.map { "\($0.name)=\($0.area())" }.joined(separator: ",")
lines.append("shapes \(names) total=\(totalArea([Square(side: 1), Square(side: 2)]))")

// Classes, dynamic dispatch, deinit.
do {
    let animals: [Animal] = [Animal(label: "generic"), Dog(label: "rex")]
    lines.append("animals " + animals.map { "\($0.label):\($0.sound())" }.joined(separator: " "))
}
lines.append("released \(Animal.released)")

// Dynamic casts.
let values: [Any] = [42, "text", 2.5, Dog(label: "fido"), [1, 2, 3]]
var kinds: [String] = []
for value in values {
    switch value {
    case let number as Int: kinds.append("Int(\(number))")
    case let text as String: kinds.append("String(\(text))")
    case let real as Double: kinds.append("Double(\(real))")
    case let animal as Animal: kinds.append("Animal(\(animal.sound()))")
    case let array as [Int]: kinds.append("Array(\(array.count))")
    default: kinds.append("?")
    }
}
lines.append("casts " + kinds.joined(separator: " "))

// Errors.
var outcomes: [String] = []
for input in [21, -4, 5000] {
    do { outcomes.append("ok(\(try doubled(input)))") }
    catch Failure.negative(let value) { outcomes.append("negative(\(value))") }
    catch { outcomes.append("error(\(error))") }
}
lines.append("errors " + outcomes.joined(separator: " "))

// Strings.
let greeting = "Grüße, 世界!"
lines.append("string count=\(greeting.count) utf8=\(greeting.utf8.count) upper=\(greeting.uppercased()) reversed=\(String(greeting.reversed()))")
let words = "the quick brown fox jumps over the lazy dog".split(separator: " ")
lines.append("words \(words.count) longest=\(words.max { $0.count < $1.count }!)")

// Collections (dictionary and set order is hashed, so sort before printing).
var counts: [String: Int] = [:]
for word in words { counts[String(word), default: 0] += 1 }
let summary = counts.keys.sorted().map { "\($0):\(counts[$0]!)" }.joined(separator: ",")
lines.append("dictionary \(summary)")
let primes = Set([2, 3, 5, 7, 11, 13]).intersection(Set(1...10))
lines.append("set \(primes.sorted())")
var numbers = Array(1...20)
numbers.removeAll { $0 % 3 == 0 }
lines.append("array \(numbers.filter { $0 % 2 == 0 }.map { $0 * $0 }.reduce(0, +)) sorted=\(numbers.sorted(by: >).prefix(3))")

// Generic structs and closures capturing state.
let box = Box(value: 20).map { $0 + 1 }.map { "value \($0)" }
var counter = 0
let increment = { counter += 7 }
for _ in 0..<6 { increment() }
lines.append("generic \(box.value) closure=\(counter)")
lines.append("optional \(Int("123").map { $0 + 1 } ?? -1) \(Int("x") ?? -1)")

for line in lines { print(line) }
print("Swift runtime: done")
