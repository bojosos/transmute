#pragma once

#include "alchemy/process_context.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>

namespace alchemy {
// libdispatch: counting semaphores, deadlines, serial/concurrent/main queues
// (async, sync, after, barriers, suspension), groups, dispatch_apply and
// timer/data sources. Queue work runs on pooled guest worker threads.
void install_darwin_dispatch(ProcessContext& context);

// Main dispatch queue service for the GUI run loop (main thread only, inside a
// synthetic call): runs at most `limit` pending main-queue items in order and
// returns how many ran. Zero when the process has no dispatch provider.
std::size_t drain_dispatch_main_queue(ProcessContext& context,
                                      std::size_t limit = std::numeric_limits<std::size_t>::max());
// Number of pending main-queue items (any thread). A run loop iteration
// drains this many, so work queued meanwhile waits for the next iteration.
[[nodiscard]] std::size_t dispatch_main_queue_length(ProcessContext& context);

// Objective-C classes of dispatch objects (OS_dispatch_queue_serial, ...):
// under OS_OBJECT_USE_OBJC a dispatch object is an Objective-C object whose
// first word is its isa, retained and released by ARC through objc_retain /
// objc_release. Registering them rewrites the isa of every live queue,
// group, source, semaphore and queue attribute; later objects are created
// with it. A zero entry keeps that kind's private marker word.
struct DispatchObjectClasses {
    std::uint64_t serial_queue{}, concurrent_queue{}, main_queue{}, global_queue{};
    std::uint64_t queue_attribute{}, group{}, source{}, semaphore{};
};
void set_dispatch_object_classes(ProcessContext& context, const DispatchObjectClasses& classes);
// A live dispatch object's kind ("queue-serial", "queue-concurrent",
// "queue-main", "queue-global", "queue-attr", "group", "source",
// "semaphore"), label (queues; the source type) and reference count, for
// -description and -retainCount. nullopt for anything else.
struct DispatchObjectInfo {
    std::string kind, label;
    std::uint64_t references{};
    bool immortal{};
};
[[nodiscard]] std::optional<DispatchObjectInfo> dispatch_object_info(ProcessContext& context, std::uint64_t object);

// dispatch_retain/dispatch_release of dispatch object kinds other providers
// implement (dispatch data, I/O channels): the handler retains (`retain`) or
// releases `object` and returns true, or returns false for an object it does
// not own. dispatch_retain/dispatch_release ask the handlers newest first,
// then the queues, groups, sources and semaphores of the dispatch provider.
// Handlers run outside the provider's locks and may call back into it.
using DispatchReferenceHandler = std::function<bool(std::uint64_t object, bool retain)>;
void add_dispatch_reference_handler(ProcessContext& context, DispatchReferenceHandler handler);
} // namespace alchemy
