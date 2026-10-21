#include "alchemy/darwin/kit/support/support_kit.hpp"

// -[NSNotificationCenter addObserverForName:object:queue:usingBlock:] on top
// of Foundation's selector-based observers (foundation_extra.cpp delivers
// synchronously and does not retain observers). Each registration is a host
// observer object owning a _Block_copy of the block; the center keeps it
// alive (as macOS does) until -removeObserver: names it. Delivery: queue nil
// runs the block on the posting thread; the main queue runs it at once on the
// main thread or, from another thread, on the main dispatch queue; other
// operation queues fail explicitly.
namespace alchemy::gui::support_kit {
namespace {

struct BlockObserver final : ObjectPayload {
    Address block{};   // _Block_copy'd
    bool main_queue{};
};
struct Delivery { Address block{}, notification{}; };

} // namespace

void install_notification_blocks(Support& support) {
    const auto center_class = support.objc.find_class("NSNotificationCenter");
    if (!center_class) return;
    auto* s = &support;
    const auto observer_class = support.declare_class("_AlchemyNotificationBlockObserver", "NSObject");
    const auto center = support.send(center_class, "defaultCenter").x0;
    if (!center) return;
    const auto fire_selector = support.objc.selector("alchemyNotificationFire:");

    const auto invoke = [s](Address block, Address notification) {
        auto& memory = s->process.calling_engine();
        CallRegs registers;
        registers.x[0] = block;
        registers.x[1] = notification;
        static_cast<void>(EngineAdapter(memory).call_regs(memory.read64(block + 16), registers));
    };
    // Main-queue delivery from another thread (dispatch_async_f context).
    static_cast<void>(support.process.install_function("__alchemy_support_notification_deliver", [s, invoke](CpuState& c, GuestMemory& m) {
        const auto block = m.read64(c.x[0]), notification = m.read64(c.x[0] + 8);
        s->process.release_guest(c.x[0]);
        invoke(block, notification);
        s->release(notification);
        static_cast<void>(s->call(libsystem_library, "__Block_release", {block}));
    }, "alchemy.internal"));

    support.method(observer_class, "alchemyNotificationFire:", [s, invoke](CpuState& c, GuestMemory& m) {
        const auto state = s->objc.find_host_state<BlockObserver>(c.x[0]);
        if (!state || !state->block) return;
        if (!state->main_queue || s->on_main_thread()) { invoke(state->block, c.x[2]); return; }
        const auto context = s->process.allocate_guest(16, 16);
        m.write64(context, s->call(libsystem_library, "__Block_copy", {state->block}));
        m.write64(context + 8, s->retain(c.x[2]));
        const auto deliver = s->process.resolve("alchemy.internal", "__alchemy_support_notification_deliver");
        const auto main_queue = s->process.resolve(libsystem_library, "__dispatch_main_q");
        if (!deliver || !main_queue) fail("main-queue notification delivery is unavailable");
        static_cast<void>(s->call(libsystem_library, "_dispatch_async_f", {main_queue->address, context, deliver->address}));
    }, "v@:@");

    support.method(center_class, "addObserverForName:object:queue:usingBlock:", [s, observer_class, fire_selector](CpuState& c, GuestMemory&) {
        const auto self = c.x[0], name = c.x[2], object = c.x[3], queue = c.x[4], block = c.x[5];
        if (!block) fail("addObserverForName:object:queue:usingBlock: requires a block");
        bool main_queue = false;
        if (queue) {
            const auto queue_class = s->objc.find_class("NSOperationQueue");
            const auto main = queue_class ? s->send(queue_class, "mainQueue").x0 : 0;
            if (queue != main) unsupported("block notification observers on an operation queue other than nil or the main queue");
            main_queue = true;
        }
        auto state = std::make_shared<BlockObserver>();
        state->block = s->call(libsystem_library, "__Block_copy", {block});
        state->main_queue = main_queue;
        std::weak_ptr<Support> owner = s->shared_from_this();
        const auto observer = s->create(observer_class, state, [owner](Address self) {
            const auto keeper = owner.lock();
            if (!keeper) return;
            if (const auto value = keeper->objc.find_host_state<BlockObserver>(self); value && value->block)
                static_cast<void>(keeper->call(libsystem_library, "__Block_release", {std::exchange(value->block, 0)}));
        });
        CallRegs add;
        add.x[2] = observer;
        add.x[3] = fire_selector;
        add.x[4] = name;
        add.x[5] = object;
        static_cast<void>(s->send(self, "addObserver:selector:name:object:", add));
        c.x[0] = observer;   // the center's reference: released by -removeObserver:
    }, "@@:@@@@?");

    // Foundation's -removeObserver: and -removeObserver:name:object: (with no
    // name and object) send this to the observer they removed: the center's
    // reference, taken by -addObserverForName:object:queue:usingBlock:, ends.
    support.method(observer_class, "_alchemyObserverRemoved", [s](CpuState& c, GuestMemory&) { s->release(c.x[0]); }, "v@:");
}

} // namespace alchemy::gui::support_kit
