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
