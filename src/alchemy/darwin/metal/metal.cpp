#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/darwin/metal/bridge.hpp"

#include <array>
#include <cstdlib>
#include <deque>
#include <map>

// Metal on Vulkan (owner: A5): Objective-C runtime helpers and
// guest-visible completion (the installer is install.cpp).
//
// install_metal_gui only registers classes, methods and functions: the
// Vulkan instance and device are created by the first
// MTLCreateSystemDefaultDevice / MTLCopyAllDevices call, and the completion
// thread by the first command-buffer commit.
namespace alchemy::metal::bridge {
namespace {

std::unique_ptr<Runtime> instance;

} // namespace

Runtime& runtime() {
    if (!instance) throw std::runtime_error("Alchemy Metal is not installed");
    return *instance;
}

void set_runtime(std::unique_ptr<Runtime> value) { instance = std::move(value); }

Address Runtime::define_class(std::string_view name, std::string_view superclass, bool publish) {
    auto cls = objc.find_class(name);
    if (!cls) {
        const auto parent = objc.find_class(superclass);
        if (!parent) throw std::runtime_error("Alchemy Metal: missing superclass " + std::string(superclass));
        cls = objc.register_native_class(std::string(name), parent, 8);
    }
    if (publish) objc.publish_class(name, cls, metal_library);
    return cls;
}

void Runtime::method(Address cls, std::string_view selector, Handler handler, bool class_method, std::string types,
                     const std::source_location& where) {
    registration_audit::Site site(where);
    static_cast<void>(objc.add_native_method(cls, std::string(selector), std::move(handler), class_method, std::move(types)));
}

void Runtime::install_dealloc(Address cls) {
    method(cls, "dealloc", [this, cls](CpuState& s, GuestMemory&) {
        const auto object = s.x[0];
        if (const auto held = objc.find_host_state<Held>(object))
            for (const auto reference : held->take()) release(reference);
        static_cast<void>(objc.send_super(object, cls, objc.selector("dealloc")));
    }, false, "v@:");
}

void install_label(Address cls) {
    auto& r = runtime();
    r.method(cls, "label", [](CpuState& s, GuestMemory&) {
        const auto held = runtime().objc.find_host_state<Held>(s.x[0]);
        if (!held) { s.x[0] = 0; return; }
        std::lock_guard lock(held->mutex);
        s.x[0] = held->label;
    });
    r.method(cls, "setLabel:", [](CpuState& s, GuestMemory&) {
        auto& r = runtime();
        const auto held = r.objc.find_host_state<Held>(s.x[0]);
        if (!held) throw std::runtime_error("Alchemy Metal: setLabel: on an object without Metal state");
        const auto label = s.x[2] ? r.cf.copy(s.x[2]) : 0;   // copy property
        Address previous{};
        {
            std::lock_guard lock(held->mutex);
            previous = held->label;
            held->label = label;
        }
        r.release(previous);
    }, false, "v@:@");
}

Address Runtime::symbol(std::string_view name) const {
    const auto value = process.resolve(system_library, name);
    if (!value) throw std::runtime_error("Alchemy Metal requires " + std::string(name));
    return value->address;
}

std::uint64_t Runtime::call(std::string_view name, std::initializer_list<std::uint64_t> arguments) {
    return EngineAdapter(process.calling_engine()).call(symbol(name), std::span(arguments.begin(), arguments.size()));
}

Address Runtime::copy_block(Address block) {
    if (!block) throw std::runtime_error("Alchemy Metal: nil block");
    return call("__Block_copy", {block});
}

void Runtime::invoke_block(Address block, std::initializer_list<std::uint64_t> arguments, bool release_block) {
    if (!block) return;
    // Block literal ABI: {isa, flags, reserved, invoke, descriptor, ...}.
    const auto invoke = process.calling_engine().read64(block + 16);
    std::vector<std::uint64_t> values{block};
    values.insert(values.end(), arguments.begin(), arguments.end());
    static_cast<void>(EngineAdapter(process.calling_engine()).call(invoke, values));
    if (release_block) static_cast<void>(call("__Block_release", {block}));
}

// ---- completion -------------------------------------------------------------------------------
//
// Completed handlers, scheduled handlers and shared-event listeners run on
// the run loop's completion thread (gui::start_completion_thread), started
// lazily from the first commit. One work item per committed command buffer
// waits on its timeline value (polling for process stops) and then invokes
// its handlers in commit order. Without a completion thread (no run loop
// provider) committing work fails explicitly.

namespace {

struct Engine {
    std::mutex mutex;
    std::shared_ptr<gui::CompletionThread> thread;
    // `queue`: the listener's dispatch queue (retained), 0 for none.
    struct Waiting { Address queue{}; std::uint64_t value{}; Address block{}; };
    std::map<Address, std::vector<Waiting>> listeners;
    // Notifications on their way to a listener's queue, by dispatch_async_f context.
    struct Queued { Address event{}, queue{}, block{}; std::uint64_t value{}; };
    std::map<std::uint64_t, Queued> queued;
    std::uint64_t next_queued{1};
    Address listener_entry{};   // the dispatch_async_f function (install_listener_dispatch)
};

Engine& engine() {
    static Engine value;
    return value;
}

std::shared_ptr<gui::CompletionThread> completion_thread() {
    auto& e = engine();
    {
        std::lock_guard lock(e.mutex);
        if (e.thread) return e.thread;
    }
    static std::mutex start_mutex;   // one thread is started, even when two threads commit first
    std::lock_guard start(start_mutex);
    {
        std::lock_guard lock(e.mutex);
        if (e.thread) return e.thread;
    }
    std::shared_ptr<gui::CompletionThread> thread;
    try {
        thread = gui::start_completion_thread(runtime().process, "com.alchemy.metal.completion");
    } catch (const std::exception& failure) {
        throw std::runtime_error(std::string("Alchemy Metal: no completion thread for command-buffer handlers (") +
                                 failure.what() + ")");
    }
    if (!thread) throw std::runtime_error("Alchemy Metal: no completion thread for command-buffer handlers");
    std::lock_guard lock(e.mutex);
    e.thread = thread;
    return thread;
}

void run_completed(const Completion& completion) {
    auto& r = runtime();
    for (const auto block : completion.completed) r.invoke_block(block, {completion.command_buffer});
    for (const auto event : completion.signaled_events) check_listeners(event);
    r.release(completion.command_buffer);
}

// MTLSharedEventNotificationBlock(event, value). With a queue, the
// completion thread hands it to dispatch_async_f (a guest call, so not from
// whichever host thread noticed the value); the entry runs the block on the
// queue and drops the references taken in add_listener.
void fire(Address event, std::uint64_t value, Address block, Address queue) {
    if (!queue) { completion_thread()->post_block(block, {event, value}); return; }
    completion_thread()->post([event, value, block, queue] {
        auto& r = runtime();
        auto& e = engine();
        std::uint64_t id{};
        {
            std::lock_guard lock(e.mutex);
            id = e.next_queued++;
            e.queued[id] = {event, queue, block, value};
        }
        static_cast<void>(r.call("_dispatch_async_f", {queue, id, e.listener_entry}));
    });
}

} // namespace

void wait_for(const std::function<bool(std::uint64_t)>& wait, std::string_view operation) {
    auto& process = runtime().process;
    for (;;) {
        if (wait(10'000'000)) return;
        auto& calling = process.calling_engine();
        if (engine_stop_requested(calling)) EngineAdapter::stop_thread(calling, operation);
    }
}

void command_buffer_committed(Completion completion) {
    auto& r = runtime();
    const auto thread = completion_thread();
    r.retain(completion.command_buffer);   // released after the completed handlers ran
    for (const auto block : completion.scheduled) thread->post_block(block, {completion.command_buffer});
    completion.scheduled.clear();
    auto shared = std::make_shared<Completion>(std::move(completion));
    thread->post([shared] {
        auto& process = runtime().process;
        for (;;) {
            if (shared->commands->wait(10'000'000)) break;
            // A stopping process drops the work (the thread releases posted blocks).
            if (engine_stop_requested(process.calling_engine())) return;
        }
        run_completed(*shared);
    });
}

void add_listener(Address event, Address listener, std::uint64_t value, Address block) {
    auto& r = runtime();
    const auto copied = r.copy_block(block);
    // The queue and the event live until the block ran (released by the entry).
    Address queue{};
    if (listener) queue = r.objc.send(listener, "dispatchQueue", CallRegs{}).x0;
    if (queue) {
        static_cast<void>(r.retain(queue));
        static_cast<void>(r.retain(event));
    }
    if (event_of(event)->signaled_value() >= value) { fire(event, value, copied, queue); return; }
    std::lock_guard lock(engine().mutex);
    engine().listeners[event].push_back({queue, value, copied});
}

void check_listeners(Address event) {
    std::vector<Engine::Waiting> due;
    {
        const auto current = event_of(event)->signaled_value();
        std::lock_guard lock(engine().mutex);
        const auto found = engine().listeners.find(event);
        if (found == engine().listeners.end()) return;
        auto& list = found->second;
        for (auto it = list.begin(); it != list.end();) {
            if (it->value <= current) { due.push_back(*it); it = list.erase(it); }
            else ++it;
        }
    }
    for (const auto& waiting : due) fire(event, waiting.value, waiting.block, waiting.queue);
}

void run_block_later(Address block, std::vector<std::uint64_t> arguments) {
    completion_thread()->post_block(block, std::move(arguments));
}

void install_listener_dispatch(Runtime& r) {
    engine().listener_entry = r.process.install_function("__alchemy_metal_shared_event_listener", [](CpuState& s, GuestMemory&) {
        auto& r = runtime();
        Engine::Queued item;
        {
            std::lock_guard lock(engine().mutex);
            const auto found = engine().queued.find(s.x[0]);
            if (found == engine().queued.end()) throw std::runtime_error("Alchemy Metal: unknown shared-event notification");
            item = found->second;
            engine().queued.erase(found);
        }
        r.invoke_block(item.block, {item.event, item.value});
        r.release(item.event);
        r.release(item.queue);
    }, std::string(metal_library));
}

} // namespace alchemy::metal::bridge
