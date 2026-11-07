// Project-authored Swift workload for the open-source stdlib modules beyond
// the core: _Concurrency (async main, tasks, async let, task groups, actors,
// AsyncStream, sleeping on both clocks, task-locals, cancellation, checked
// continuations, MainActor), Observation, Synchronization and Darwin.

import Darwin
import Observation
import Synchronization

actor Counter {
    var value = 0
    func increment() -> Int { value += 1; return value }
}

enum Trace {
    @TaskLocal static var label = "none"
}

// Noncopyable, so global (escaping closures reach them without capturing).
let changes = Mutex(0)
let hits = Atomic(0)

@Observable final class Model {
    var title = "first"
    var count = 0
}

func square(_ n: Int) async -> Int {
    await Task.yield()
    return n * n
}

@MainActor func onMain() -> String { "main actor" }

@main
struct SwiftConcurrency {
    static func main() async {
        let task = Task { await square(7) }
        print("task", await task.value)

        async let a = square(3)
        async let b = square(4)
        print("async let", await a + b)

        let total = await withTaskGroup(of: Int.self) { group in
            for i in 1...10 { group.addTask { await square(i) } }
            var sum = 0
            for await value in group { sum += value }
            return sum
        }
        print("group", total)

        let counter = Counter()
        await withTaskGroup(of: Void.self) { group in
            for _ in 0..<100 { group.addTask { _ = await counter.increment() } }
        }
        print("actor", await counter.value)

        let stream = AsyncStream<Int> { continuation in
            Task {
                for i in 0..<5 { continuation.yield(i) }
                continuation.finish()
            }
        }
        var streamed = [Int]()
        for await value in stream { streamed.append(value) }
        print("stream", streamed)

        let continuous = ContinuousClock.now
        try? await Task.sleep(for: .milliseconds(30))
        let continuousElapsed = ContinuousClock.now - continuous
        let suspending = SuspendingClock.now
        try? await Task.sleep(until: suspending + .milliseconds(30), clock: .suspending)
        let suspendingElapsed = SuspendingClock.now - suspending
        print("sleep", continuousElapsed >= .milliseconds(30) && continuousElapsed < .seconds(5),
              suspendingElapsed >= .milliseconds(30) && suspendingElapsed < .seconds(5))

        await Trace.$label.withValue("outer") {
            let inner = await Task { Trace.label }.value
            print("task-local", Trace.label, inner)
        }

        let cancelled = Task { () -> Bool in
            try? await Task.sleep(for: .seconds(10))
            return Task.isCancelled
        }
        cancelled.cancel()
        print("cancelled", await cancelled.value)

        let resumed = await withCheckedContinuation { (continuation: CheckedContinuation<Int, Never>) in
            Task.detached { continuation.resume(returning: 99) }
        }
        print("continuation", resumed)
        print(await onMain())

        let model = Model()
        withObservationTracking { _ = model.title } onChange: { changes.withLock { $0 += 1 } }
        model.count = 1
        model.title = "second"
        model.title = "third"
        print("observation", changes.withLock { $0 }, model.title)

        await withTaskGroup(of: Void.self) { group in
            for _ in 0..<8 { group.addTask { for _ in 0..<1000 { hits.wrappingAdd(1, ordering: .relaxed) } } }
        }
        print("atomic", hits.load(ordering: .relaxed))

        print("darwin", getpid() > 0, errno >= 0)
    }
}
