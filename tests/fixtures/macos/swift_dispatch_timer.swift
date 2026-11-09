// Project-authored Swift workload for the Dispatch overlay's timer source:
// makeTimerSource(flags:queue:), schedule(deadline:repeating:leeway:),
// DispatchTimeInterval and DispatchSource.TimerFlags (OBS Studio's camera
// extension drives its frames with them).

import Dispatch
import Foundation

@main
struct SwiftDispatchTimer {
    static func main() {
        print("interval", DispatchTimeInterval.seconds(1) == .milliseconds(1000), DispatchTimeInterval.never == .seconds(1))

        // The handler counts on the timer's queue; the main thread polls the count (raw memory, an aligned word).
        let fired = UnsafeMutablePointer<Int>.allocate(capacity: 1)
        fired.initialize(to: 0)
        let timer = DispatchSource.makeTimerSource(flags: .strict, queue: DispatchQueue(label: "timer"))
        timer.setEventHandler {
            if fired.pointee < 3 { fired.pointee += 1 }
        }
        timer.schedule(deadline: .now() + 0.01, repeating: 0.01, leeway: .milliseconds(1))
        timer.resume()
        var polls = 0
        while fired.pointee < 3 && polls < 1000 {
            usleep(10_000)
            polls += 1
        }
        timer.cancel()
        print("timer", fired.pointee)
        print("done")
    }
}
