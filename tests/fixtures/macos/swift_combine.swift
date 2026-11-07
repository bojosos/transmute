// Project-authored Swift workload for the guest Combine.framework (OpenCombine
// plus runtime/swift/combine): operators, subjects, @Published and
// ObservableObject, Future, MergeMany, Zip, errors,
// cancellation and async values.

import Combine

final class Model: ObservableObject {
    @Published var count = 0
    @Published var name = "a"
}

enum Failure: Error { case broken }

@main
struct SwiftCombine {
    static func main() async {
        var bag = Set<AnyCancellable>()

        var mapped = [Int]()
        Just(5).map { $0 * 2 }.filter { $0 > 3 }.sink { mapped.append($0) }.store(in: &bag)
        [1, 2, 3, 4].publisher.map { $0 * $0 }.reduce(0, +).sink { mapped.append($0) }.store(in: &bag)
        print("operators", mapped)

        let subject = PassthroughSubject<String, Never>()
        var received = [String]()
        subject.removeDuplicates().sink { received.append($0) }.store(in: &bag)
        ["x", "x", "y", "z", "z"].forEach { subject.send($0) }
        print("passthrough", received)

        let current = CurrentValueSubject<Int, Never>(1)
        var values = [Int]()
        let currentSink = current.sink { values.append($0) }
        current.send(2)
        currentSink.cancel()
        current.send(3)
        print("current", values, current.value)

        let model = Model()
        var changes = 0
        var counts = [Int]()
        model.objectWillChange.sink { changes += 1 }.store(in: &bag)
        model.$count.sink { counts.append($0) }.store(in: &bag)
        model.count = 1
        model.count = 2
        model.name = "b"
        print("published", counts, changes)

        var future = 0
        Future<Int, Never> { promise in promise(.success(42)) }.sink { future = $0 }.store(in: &bag)
        print("future", future)

        var merged = [Int]()
        let a = PassthroughSubject<Int, Never>(), b = PassthroughSubject<Int, Never>()
        var mergeFinished = false
        a.merge(with: b).sink(receiveCompletion: { _ in mergeFinished = true }, receiveValue: { merged.append($0) }).store(in: &bag)
        a.send(1); b.send(2); a.send(3)
        a.send(completion: .finished)
        b.send(4)
        let finishedEarly = mergeFinished
        b.send(completion: .finished)
        print("merge", merged, finishedEarly, mergeFinished)

        var many = [Int]()
        Publishers.MergeMany([1, 2].publisher, [3].publisher, [4, 5].publisher).collect().sink { many = $0 }.store(in: &bag)
        print("mergemany", many.sorted())

        var zipped = [String]()
        [1, 2, 3].publisher.zip(["a", "b"].publisher).sink { zipped.append("\($0)\($1)") }.store(in: &bag)
        print("zip", zipped)

        var outcome = ""
        Fail<Int, Failure>(error: .broken).catch { _ in Just(-1) }.sink { outcome = "caught \($0)" }.store(in: &bag)
        print("catch", outcome)

        var asyncValues = [Int]()
        for await value in [7, 8, 9].publisher.values { asyncValues.append(value) }
        print("values", asyncValues)
        print("done")
    }
}
