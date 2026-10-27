#pragma once

#include "alchemy/process_context.hpp"

#include <memory>

namespace alchemy {

// Cooperative stop of a guest worker blocked in a libSystem wait while the
// process exits. Deliberately not a std::exception: engines wrap those into
// diagnostics, while this one unwinds the worker's dispatcher unchanged and the
// worker ends without an error report. Throw it only on worker engines.
struct GuestThreadStop {};
// True when the calling engine is a worker and the process is exiting; such a
// worker should throw GuestThreadStop instead of returning to guest code.
[[nodiscard]] bool guest_worker_must_stop(ProcessContext& context);
// Whether the calling host thread unwinds GuestThreadStop: a DarwinThreads
// worker's entry catches it (GuestStopScope there). Elsewhere (an engine that
// a host or test thread drives) it would reach std::terminate.
[[nodiscard]] bool guest_stop_handled() noexcept;
struct GuestStopScope {
    GuestStopScope() noexcept;
    ~GuestStopScope();
    GuestStopScope(const GuestStopScope&) = delete;
    GuestStopScope& operator=(const GuestStopScope&) = delete;
    bool previous;
};
// For a blocking libSystem call that polls for process exit: true when the
// process is exiting, but a worker never sees it: it stops here
// (GuestThreadStop), as on Darwin, where exit ends every other thread without
// returning from its call. An error such as ECANCELED or EINTR instead reaches
// code that checks it (Chrome's message pump CHECKs kevent64's result). A
// thread that cannot unwind the stop gets the error, as before.
inline bool process_exiting(ProcessContext& context) {
    if (guest_stop_handled() && guest_worker_must_stop(context)) throw GuestThreadStop{};
    return context.requested_exit().has_value();
}

// Darwin ABI adapters over Transmute's existing shared-address-space workers.
class DarwinThreads {
public:
    explicit DarwinThreads(ProcessContext& context);
    ~DarwinThreads();
    DarwinThreads(const DarwinThreads&) = delete;
    DarwinThreads& operator=(const DarwinThreads&) = delete;
    void join_workers();
    void cleanup_thread(ThreadContext& thread);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace alchemy
