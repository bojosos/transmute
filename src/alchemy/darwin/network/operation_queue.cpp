#include "alchemy/darwin/network/network_internal.hpp"

#include <array>
#include <set>

// NSOperationQueue over libdispatch, installed only when no other provider
// defines NSOperationQueue (Alchemy's Foundation has none). NSURLSession
// delegate queues and -addOperationWithBlock: run blocks directly.
//
// NSOperation, NSBlockOperation and NSInvocationOperation (QLab group G2):
// -start runs -main between the KVO-compliant isExecuting / isFinished
// changes (a cancelled operation finishes without -main; starting one that is
// not ready fails as Foundation raises); the completion block runs on a
// global queue after the operation finishes. Dependencies make an operation
// ready (isReady, KVO-compliant) once each dependency is finished.
// -addOperation: starts an operation on the queue's dispatch queue once it is
// ready; asynchronous operations (subclasses overriding -start/-isFinished)
// count as running until they report isFinished through key-value observing.
// -operationCount, -operations, -cancelAllOperations (which cancels the
// operations) and -waitUntilAllOperationsAreFinished cover them too.
namespace alchemy::gui::network {
namespace {

struct QueueState final : ObjectPayload {
    std::mutex mutex;
    Address dispatch{};                       // +1 private dispatch queue, or the main queue
    bool main{};
    std::int64_t max_concurrent{-1};          // NSOperationQueueDefaultMaxConcurrentOperationCount
    std::int64_t quality{-1};                 // NSQualityOfServiceDefault
    Address name{};                           // +1
    std::atomic<std::uint64_t> generation{};  // bumped by -cancelAllOperations
    std::atomic<std::int64_t> pending{};
    bool suspended{};
    std::vector<Address> operations;          // NSOperation objects queued or running (+1)
};

struct OperationState final : ObjectPayload {
    std::recursive_mutex mutex;
    bool cancelled{}, executing{}, finished{}, enqueued{};
    std::vector<Address> dependencies;        // +1
    std::vector<Address> blocks;              // NSBlockOperation execution blocks (copied)
    Address completion{};                     // copied block
    Address invocation{};                     // NSInvocationOperation (+1)
    Address name{};                           // +1
    std::int64_t priority{}, quality{-1};
    std::set<Address> observed;               // dependencies whose isFinished this operation observes
};

Address queue_class{}, main_queue_object{}, noop_function{};

std::shared_ptr<QueueState> state_of(Bridge& b, Address object) {
    return object ? b.objc.find_host_state<QueueState>(object) : nullptr;
}

Address ensure_dispatch(Bridge& b, QueueState& state) {
    std::lock_guard lock(state.mutex);
    if (state.dispatch) return state.dispatch;
    const auto label = state.name ? b.text(state.name) : std::string("com.apple.NSOperationQueue");
    if (state.max_concurrent == 1) state.dispatch = b.create_serial_queue(label);
    else {
        const auto text = b.process.allocate_guest(label.size() + 1);
        b.memory().write_bytes(text, std::span(reinterpret_cast<const std::uint8_t*>(label.data()), label.size()));
        b.memory().write8(text + label.size(), 0);
        state.dispatch = b.call(security::libsystem_library, "_dispatch_queue_create",
                                {text, b.symbol(security::libsystem_library, "__dispatch_queue_attr_concurrent")});
    }
    if (state.suspended) static_cast<void>(b.call(security::libsystem_library, "_dispatch_suspend", {state.dispatch}));
    return state.dispatch;
}

} // namespace

Address dispatch_queue_of(Bridge& b, Address operation_queue) {
    if (!operation_queue) return 0;
    if (const auto state = state_of(b, operation_queue)) return ensure_dispatch(b, *state);
    if (b.objc.responds(operation_queue, b.objc.selector("underlyingQueue")))
        return b.objc.send(operation_queue, "underlyingQueue").x0;
    return 0;
}

Address make_serial_operation_queue(Bridge& b, std::string_view name) {
    if (!noop_function) {   // another provider's NSOperationQueue
        const auto object = b.objc.send(b.objc.send(queue_class, "alloc").x0, "init").x0;
        CallRegs count;
        count.x[2] = 1;
        static_cast<void>(b.objc.send(object, "setMaxConcurrentOperationCount:", count));
        return object;
    }
    const auto object = b.objc.allocate(queue_class);
    auto state = std::make_shared<QueueState>();
    state->max_concurrent = 1;
    state->name = b.string(name);
    static_cast<void>(b.objc.host_state<QueueState>(object, [&] { return state; }));
    return object;
}

namespace {

// ---- NSOperation ----------------------------------------------------------------------------------
// One host observer object receives the key-value notifications the
// operations machinery registers for (isFinished of dependencies and of
// queued asynchronous operations, isReady of waiting queued operations).
struct Observations {
    std::mutex mutex;
    Address observer{};
    std::map<Address, std::function<void(Address object)>> callbacks;   // context -> action
    Address next{1};
};

void install_operations(Bridge& b, Address queue_cls) {
    auto bridge = b.shared_from_this();
    auto observations = std::make_shared<Observations>();
    auto observer_class = b.objc.find_class("_AlchemyOperationObserver");
    if (!observer_class) observer_class = b.objc.register_native_class("_AlchemyOperationObserver", b.object_class, 8);
    b.method(observer_class, "observeValueForKeyPath:ofObject:change:context:", [observations](CpuState& s, GuestMemory&) {
        std::function<void(Address)> action;
        {
            std::lock_guard lock(observations->mutex);
            const auto found = observations->callbacks.find(s.x[5]);
            if (found == observations->callbacks.end()) return;
            action = found->second;
        }
        action(s.x[3]);
    }, false, "v@:@@@^v");
    observations->observer = b.objc.send(b.objc.send(observer_class, "alloc").x0, "init").x0;
    const auto key = [bridge](std::string_view text) { return bridge->autorelease(bridge->string(text)); };
    // Observes `key_path` of `object`; the action runs on every change. Returns the context.
    const auto observe = [bridge, observations, key](Address object, std::string_view key_path, std::function<void(Address)> action) {
        Address context;
        {
            std::lock_guard lock(observations->mutex);
            context = observations->next++;
            observations->callbacks.emplace(context, std::move(action));
        }
        CallRegs registers;
        registers.x[2] = observations->observer;
        registers.x[3] = key(key_path);
        registers.x[4] = 1;   // NSKeyValueObservingOptionNew
        registers.x[5] = context;
        static_cast<void>(bridge->objc.send(object, "addObserver:forKeyPath:options:context:", registers));
        return context;
    };
    const auto unobserve = [bridge, observations, key](Address object, std::string_view key_path, Address context) {
        {
            std::lock_guard lock(observations->mutex);
            if (!observations->callbacks.erase(context)) return;
        }
        CallRegs registers;
        registers.x[2] = observations->observer;
        registers.x[3] = key(key_path);
        registers.x[4] = context;
        static_cast<void>(bridge->objc.send(object, "removeObserver:forKeyPath:context:", registers));
    };
    const auto flag = [bridge](Address object, std::string_view selector) {
        return (bridge->objc.send(object, selector).x0 & 0xff) != 0;
    };
    const auto notify = [bridge, key](Address object, std::initializer_list<const char*> keys, bool will) {
        for (const auto* name : keys) {
            CallRegs registers;
            registers.x[2] = key(name);
            static_cast<void>(bridge->objc.send(object, will ? "willChangeValueForKey:" : "didChangeValueForKey:", registers));
        }
    };

    const auto operation = b.define_class("NSOperation", b.object_class, foundation);
    const auto block_operation = b.define_class("NSBlockOperation", operation, foundation);
    const auto invocation_operation = b.define_class("NSInvocationOperation", operation, foundation);
    const auto state_of = [bridge](Address object) { return bridge->objc.host_state<OperationState>(object); };

    b.method(operation, "init", [state_of](CpuState& s, GuestMemory&) { static_cast<void>(state_of(s.x[0])); });
    b.method(operation, "dealloc", [bridge, operation, state_of, unobserve](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<Address> dependencies, blocks;
        Address completion, invocation, name;
        {
            std::lock_guard lock(state->mutex);
            dependencies.swap(state->dependencies);
            blocks.swap(state->blocks);
            completion = std::exchange(state->completion, 0);
            invocation = std::exchange(state->invocation, 0);
            name = std::exchange(state->name, 0);
        }
        for (const auto dependency : dependencies) bridge->release(dependency);
        for (const auto block : blocks) bridge->block_release(block);
        bridge->block_release(completion);
        bridge->release(invocation);
        bridge->release(name);
        static_cast<void>(bridge->objc.send_super(s.x[0], operation, bridge->objc.selector("dealloc")));
    }, false, "v@:");
    const auto read = [state_of](Address object, bool OperationState::*member) {
        const auto state = state_of(object);
        std::lock_guard lock(state->mutex);
        return (*state).*member;
    };
    b.method(operation, "isCancelled", [read](CpuState& s, GuestMemory&) { s.x[0] = read(s.x[0], &OperationState::cancelled); }, false, "B@:");
    b.method(operation, "isExecuting", [read](CpuState& s, GuestMemory&) { s.x[0] = read(s.x[0], &OperationState::executing); }, false, "B@:");
    b.method(operation, "isFinished", [read](CpuState& s, GuestMemory&) { s.x[0] = read(s.x[0], &OperationState::finished); }, false, "B@:");
    b.method(operation, "isConcurrent", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    b.method(operation, "isAsynchronous", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    b.method(operation, "isReady", [state_of, flag](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<Address> dependencies;
        { std::lock_guard lock(state->mutex); dependencies = state->dependencies; }
        bool ready = true;
        for (const auto dependency : dependencies) if (!flag(dependency, "isFinished")) { ready = false; break; }
        s.x[0] = ready;
    }, false, "B@:");
    b.method(operation, "cancel", [state_of, notify](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        { std::lock_guard lock(state->mutex); if (state->cancelled || state->finished) return; }
        notify(s.x[0], {"isCancelled", "isReady"}, true);
        { std::lock_guard lock(state->mutex); state->cancelled = true; }
        notify(s.x[0], {"isCancelled", "isReady"}, false);
    }, false, "v@:");
    b.method(operation, "main", [](CpuState&, GuestMemory&) {}, false, "v@:");   // subclasses override
    b.method(operation, "start", [bridge, state_of, flag, notify](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = state_of(self);
        {
            std::lock_guard lock(state->mutex);
            if (state->executing || state->finished) security::fail("NSInvalidArgumentException: -[NSOperation start] of an operation that already started");
        }
        const bool cancelled = flag(self, "isCancelled");
        if (!cancelled && !flag(self, "isReady")) security::fail("NSInvalidArgumentException: -[NSOperation start] of an operation that is not ready");
        if (!cancelled) {
            notify(self, {"isExecuting"}, true);
            { std::lock_guard lock(state->mutex); state->executing = true; }
            notify(self, {"isExecuting"}, false);
            static_cast<void>(bridge->objc.send(self, "main"));
        }
        notify(self, {"isExecuting", "isFinished"}, true);
        Address completion;
        {
            std::lock_guard lock(state->mutex);
            state->executing = false;
            state->finished = true;
            completion = state->completion;
        }
        notify(self, {"isExecuting", "isFinished"}, false);
        // The completion block runs on a global queue once the operation finished.
        if (completion) {
            const auto block = bridge->block_copy(completion);
            const auto global = bridge->call(security::libsystem_library, "_dispatch_get_global_queue", {0, 0});
            bridge->dispatch_async(global, [bridge, block] { static_cast<void>(bridge->invoke_block(block, {})); bridge->block_release(block); });
        }
    }, false, "v@:");
    b.method(operation, "waitUntilFinished", [bridge, flag](CpuState& s, GuestMemory&) {
        while (!flag(s.x[0], "isFinished")) static_cast<void>(bridge->call(security::libsystem_library, "_usleep", {1000}));
    }, false, "v@:");
    b.method(operation, "addDependency:", [bridge, state_of, flag, notify, observe](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], dependency = s.x[2];
        if (!dependency) security::fail("NSInvalidArgumentException: -[NSOperation addDependency:] with nil");
        const auto state = state_of(self);
        bool first_observation{};
        {
            std::lock_guard lock(state->mutex);
            if (std::find(state->dependencies.begin(), state->dependencies.end(), dependency) != state->dependencies.end()) return;
            first_observation = state->observed.insert(dependency).second;
        }
        notify(self, {"isReady", "dependencies"}, true);
        { std::lock_guard lock(state->mutex); state->dependencies.push_back(bridge->retain(dependency)); }
        notify(self, {"isReady", "dependencies"}, false);
        // isReady changes when a dependency finishes.
        if (first_observation)
            static_cast<void>(observe(dependency, "isFinished", [notify, self](Address) {
                notify(self, {"isReady"}, true);
                notify(self, {"isReady"}, false);
            }));
        static_cast<void>(flag);
    }, false, "v@:@");
    b.method(operation, "removeDependency:", [bridge, state_of, notify](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        bool found{};
        { std::lock_guard lock(state->mutex); found = std::find(state->dependencies.begin(), state->dependencies.end(), s.x[2]) != state->dependencies.end(); }
        if (!found) return;
        notify(s.x[0], {"isReady", "dependencies"}, true);
        { std::lock_guard lock(state->mutex); std::erase(state->dependencies, s.x[2]); }
        notify(s.x[0], {"isReady", "dependencies"}, false);
        bridge->release(s.x[2]);
    }, false, "v@:@");
    b.method(operation, "dependencies", [bridge, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<Address> values;
        { std::lock_guard lock(state->mutex); values = state->dependencies; }
        s.x[0] = bridge->autorelease(bridge->array(values));
    });
    b.method(operation, "completionBlock", [state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->completion;
    }, false, "@?@:");
    b.method(operation, "setCompletionBlock:", [bridge, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto copy = bridge->block_copy(s.x[2]);
        Address old;
        { std::lock_guard lock(state->mutex); old = std::exchange(state->completion, copy); }
        bridge->block_release(old);
    }, false, "v@:@?");
    b.method(operation, "name", [state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->name;
    });
    b.method(operation, "setName:", [bridge, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto copy = s.x[2] ? bridge->objc.send(s.x[2], "copy").x0 : 0;
        Address old;
        { std::lock_guard lock(state->mutex); old = std::exchange(state->name, copy); }
        bridge->release(old);
    }, false, "v@:@");
    b.method(operation, "queuePriority", [state_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state_of(s.x[0])->priority); }, false, "q@:");
    b.method(operation, "setQueuePriority:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->priority = static_cast<std::int64_t>(s.x[2]); }, false, "v@:q");
    b.method(operation, "qualityOfService", [state_of](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(state_of(s.x[0])->quality); }, false, "q@:");
    b.method(operation, "setQualityOfService:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->quality = static_cast<std::int64_t>(s.x[2]); }, false, "v@:q");

    // NSBlockOperation: the blocks run in order on the operation's thread.
    b.method(block_operation, "blockOperationWithBlock:", [bridge](CpuState& s, GuestMemory&) {
        const auto object = bridge->objc.send(bridge->objc.send(s.x[0], "alloc").x0, "init").x0;
        CallRegs registers;
        registers.x[2] = s.x[2];
        static_cast<void>(bridge->objc.send(object, "addExecutionBlock:", registers));
        s.x[0] = bridge->autorelease(object);
    }, true, "@@:@?");
    b.method(block_operation, "addExecutionBlock:", [bridge, state_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) security::fail("NSInvalidArgumentException: -[NSBlockOperation addExecutionBlock:] with a nil block");
        const auto state = state_of(s.x[0]);
        std::lock_guard lock(state->mutex);
        if (state->executing || state->finished)
            security::fail("NSInvalidArgumentException: -[NSBlockOperation addExecutionBlock:] after the operation started");
        state->blocks.push_back(bridge->block_copy(s.x[2]));
    }, false, "v@:@?");
    b.method(block_operation, "executionBlocks", [bridge, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<Address> blocks;
        { std::lock_guard lock(state->mutex); blocks = state->blocks; }
        s.x[0] = bridge->autorelease(bridge->array(blocks));
    });
    b.method(block_operation, "main", [bridge, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        std::vector<Address> blocks;
        { std::lock_guard lock(state->mutex); blocks = state->blocks; }
        for (const auto block : blocks) static_cast<void>(bridge->invoke_block(block, {}));
    }, false, "v@:");

    // NSInvocationOperation: an NSInvocation of target/selector/object.
    b.method(invocation_operation, "initWithInvocation:", [bridge, state_of](CpuState& s, GuestMemory&) {
        if (!s.x[2]) security::fail("NSInvalidArgumentException: -[NSInvocationOperation initWithInvocation:] with nil");
        const auto state = state_of(s.x[0]);
        static_cast<void>(bridge->objc.send(s.x[2], "retainArguments"));
        state->invocation = bridge->retain(s.x[2]);
    }, false, "@@:@");
    b.method(invocation_operation, "initWithTarget:selector:object:", [bridge](CpuState& s, GuestMemory&) {
        const auto self = s.x[0], target = s.x[2], selector = s.x[3], argument = s.x[4];
        CallRegs query;
        query.x[2] = selector;
        const auto signature = bridge->objc.send(target, "methodSignatureForSelector:", query).x0;
        if (!signature) { bridge->release(self); s.x[0] = 0; return; }   // the target does not respond: nil, as Foundation
        const auto invocation_class = bridge->objc.find_class("NSInvocation");
        CallRegs make;
        make.x[2] = signature;
        const auto invocation = bridge->objc.send(invocation_class, "invocationWithMethodSignature:", make).x0;
        CallRegs set;
        set.x[2] = target;
        static_cast<void>(bridge->objc.send(invocation, "setTarget:", set));
        set.x[2] = selector;
        static_cast<void>(bridge->objc.send(invocation, "setSelector:", set));
        if (bridge->objc.send(signature, "numberOfArguments").x0 > 2) {
            const auto cell = bridge->process.allocate_guest(8);
            bridge->memory().write64(cell, argument);
            CallRegs argument_registers;
            argument_registers.x[2] = cell;
            argument_registers.x[3] = 2;
            static_cast<void>(bridge->objc.send(invocation, "setArgument:atIndex:", argument_registers));
            bridge->process.release_guest(cell);
        }
        CallRegs init;
        init.x[2] = invocation;
        s.x[0] = bridge->objc.send(self, "initWithInvocation:", init).x0;
    }, false, "@@:@:@");
    b.method(invocation_operation, "invocation", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->invocation; });
    b.method(invocation_operation, "main", [bridge, state_of](CpuState& s, GuestMemory&) {
        if (const auto invocation = state_of(s.x[0])->invocation) static_cast<void>(bridge->objc.send(invocation, "invoke"));
    }, false, "v@:");
    b.method(invocation_operation, "result", [bridge, state_of](CpuState& s, GuestMemory&) {
        const auto invocation = state_of(s.x[0])->invocation;
        if (!invocation) { s.x[0] = 0; return; }
        const auto signature = bridge->objc.send(invocation, "methodSignature").x0;
        const auto type = bridge->objc.send(signature, "methodReturnType").x0;
        if (bridge->memory().read8(type) != '@') security::fail("NSInvalidArgumentException: -[NSInvocationOperation result] of a non-object result");
        const auto cell = bridge->process.allocate_guest(8);
        CallRegs get;
        get.x[2] = cell;
        static_cast<void>(bridge->objc.send(invocation, "getReturnValue:", get));
        s.x[0] = bridge->memory().read64(cell);
        bridge->process.release_guest(cell);
    });

    // ---- NSOperationQueue -addOperation: ----
    const auto queue_state = [bridge](Address object) {
        auto state = bridge->objc.find_host_state<QueueState>(object);
        if (!state) security::fail("NSOperationQueue is not initialized");
        return state;
    };
    struct Runner {
        std::function<void(Address queue_object, Address op)> start;
    };
    auto runner = std::make_shared<Runner>();
    // Ends the queue's ownership of a finished operation.
    const auto retire = [bridge, queue_state](Address queue_object, Address op) {
        const auto state = queue_state(queue_object);
        bool owned{};
        {
            std::lock_guard lock(state->mutex);
            const auto found = std::find(state->operations.begin(), state->operations.end(), op);
            if (found != state->operations.end()) { state->operations.erase(found); owned = true; }
        }
        if (!owned) return;
        --state->pending;
        bridge->release(op);
        bridge->release(queue_object);
    };
    runner->start = [bridge, queue_state, flag, observe, unobserve, retire](Address queue_object, Address op) {
        const auto state = queue_state(queue_object);
        const auto dispatch = ensure_dispatch(*bridge, *state);
        bridge->dispatch_async(dispatch, [bridge, flag, observe, unobserve, retire, queue_object, op] {
            static_cast<void>(bridge->objc.send(op, "start"));
            if (flag(op, "isFinished")) { retire(queue_object, op); return; }
            // An asynchronous operation finishes later: watch isFinished.
            auto context = std::make_shared<Address>();
            *context = observe(op, "isFinished", [bridge, flag, unobserve, retire, queue_object, op, context](Address) {
                if (!flag(op, "isFinished")) return;
                const auto global = bridge->call(security::libsystem_library, "_dispatch_get_global_queue", {0, 0});
                bridge->dispatch_async(global, [unobserve, retire, queue_object, op, context] {
                    unobserve(op, "isFinished", *context);
                    retire(queue_object, op);
                });
            });
            if (flag(op, "isFinished")) { unobserve(op, "isFinished", *context); retire(queue_object, op); }
        });
    };
    const auto enqueue = [bridge, queue_state, flag, observe, unobserve, runner](Address queue_object, Address op) {
        if (!op) security::fail("NSInvalidArgumentException: -[NSOperationQueue addOperation:] with nil");
        if (const auto op_state = bridge->objc.find_host_state<OperationState>(op)) {
            std::lock_guard lock(op_state->mutex);
            if (op_state->enqueued || op_state->executing || op_state->finished)
                security::fail("NSInvalidArgumentException: operation is already enqueued, executing or finished");
            op_state->enqueued = true;
        }
        const auto state = queue_state(queue_object);
        {
            std::lock_guard lock(state->mutex);
            state->operations.push_back(bridge->retain(op));
        }
        static_cast<void>(bridge->retain(queue_object));
        ++state->pending;
        if (flag(op, "isReady")) { runner->start(queue_object, op); return; }
        // Not ready yet: start once isReady turns YES.
        auto context = std::make_shared<Address>();
        auto started = std::make_shared<std::atomic<bool>>(false);
        const auto try_start = [bridge, flag, unobserve, runner, queue_object, op, context, started] {
            if (!flag(op, "isReady") || started->exchange(true)) return;
            const auto global = bridge->call(security::libsystem_library, "_dispatch_get_global_queue", {0, 0});
            bridge->dispatch_async(global, [unobserve, op, context] { unobserve(op, "isReady", *context); });
            runner->start(queue_object, op);
        };
        *context = observe(op, "isReady", [try_start](Address) { try_start(); });
        try_start();
    };
    b.method(queue_cls, "addOperation:", [enqueue](CpuState& s, GuestMemory&) { enqueue(s.x[0], s.x[2]); }, false, "v@:@");
    b.method(queue_cls, "addOperations:waitUntilFinished:", [bridge, enqueue, flag](CpuState& s, GuestMemory&) {
        const auto list = s.x[2];
        const auto count = list ? bridge->objc.send(list, "count").x0 : 0;
        std::vector<Address> ops;
        for (Address index = 0; index < count; ++index) {
            CallRegs at;
            at.x[2] = index;
            ops.push_back(bridge->objc.send(list, "objectAtIndex:", at).x0);
        }
        for (const auto op : ops) enqueue(s.x[0], op);
        if (s.x[3] & 0xff)
            for (const auto op : ops) while (!flag(op, "isFinished")) static_cast<void>(bridge->call(security::libsystem_library, "_usleep", {1000}));
    }, false, "v@:@B");
    b.method(queue_cls, "operations", [bridge, queue_state](CpuState& s, GuestMemory&) {
        const auto state = queue_state(s.x[0]);
        std::vector<Address> values;
        { std::lock_guard lock(state->mutex); values = state->operations; }
        s.x[0] = bridge->autorelease(bridge->array(values));
    });
}

} // namespace

void install_operation_queues(Bridge& b) {
    if (const auto existing = b.objc.find_class("NSOperationQueue"); existing && b.objc.responds(existing, b.objc.selector("mainQueue"))) {
        queue_class = existing;   // another provider owns NSOperationQueue
        return;
    }
    queue_class = b.define_class("NSOperationQueue", b.object_class, foundation);
    noop_function = b.process.install_function("__alchemy_operation_queue_noop", [](CpuState& s, GuestMemory&) { s.x[0] = 0; },
                                               std::string(security::libsystem_library));
    auto bridge = b.shared_from_this();
    const auto cls = queue_class;
    const auto require = [bridge](Address object) {
        auto state = state_of(*bridge, object);
        if (!state) security::fail("NSOperationQueue is not initialized");
        return state;
    };
    b.method(cls, "mainQueue", [bridge](CpuState& s, GuestMemory&) {
        if (!main_queue_object) {
            const auto object = bridge->process.objects().create(16, queue_class, ObjectKind::instance, {}, {}, true);
            auto state = std::make_shared<QueueState>();
            state->main = true;
            state->max_concurrent = 1;
            state->dispatch = bridge->main_queue();
            state->name = bridge->string("NSOperationQueue Main Queue");
            static_cast<void>(bridge->objc.host_state<QueueState>(object, [&] { return state; }));
            main_queue_object = object;
        }
        s.x[0] = main_queue_object;
    }, true);
    b.method(cls, "init", [bridge](CpuState& s, GuestMemory&) {
        static_cast<void>(bridge->objc.host_state<QueueState>(s.x[0]));
    });
    b.method(cls, "dealloc", [bridge, cls](CpuState& s, GuestMemory&) {
        if (const auto state = state_of(*bridge, s.x[0]); state && !state->main) {
            bridge->release(std::exchange(state->name, 0));
            if (const auto queue = std::exchange(state->dispatch, 0)) {
                if (state->suspended) static_cast<void>(bridge->call(security::libsystem_library, "_dispatch_resume", {queue}));
                static_cast<void>(bridge->call(security::libsystem_library, "_dispatch_release", {queue}));
            }
        }
        static_cast<void>(bridge->objc.send_super(s.x[0], cls, bridge->objc.selector("dealloc")));
    }, false, "v@:");
    b.method(cls, "addOperationWithBlock:", [bridge, require](CpuState& s, GuestMemory&) {
        const auto state = require(s.x[0]);
        if (!s.x[2]) security::fail("-addOperationWithBlock: with a nil block");
        const auto queue = ensure_dispatch(*bridge, *state);
        const auto block = bridge->block_copy(s.x[2]);
        const auto generation = state->generation.load();
        ++state->pending;
        bridge->dispatch_async(queue, [bridge, state, block, generation] {
            if (state->generation.load() == generation) bridge->invoke_block(block, {});
            bridge->block_release(block);
            --state->pending;
        });
    }, false, "v@:@?");
    b.method(cls, "operationCount", [require](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(std::max<std::int64_t>(0, require(s.x[0])->pending.load()));
    }, false, "q@:");
    // Pending blocks that have not started yet are skipped; operations are cancelled.
    b.method(cls, "cancelAllOperations", [bridge, require](CpuState& s, GuestMemory&) {
        const auto state = require(s.x[0]);
        ++state->generation;
        std::vector<Address> operations;
        { std::lock_guard lock(state->mutex); operations = state->operations; }
        for (const auto op : operations) static_cast<void>(bridge->objc.send(op, "cancel"));
    }, false, "v@:");
    b.method(cls, "waitUntilAllOperationsAreFinished", [bridge, require](CpuState& s, GuestMemory&) {
        const auto state = require(s.x[0]);
        const auto queue = ensure_dispatch(*bridge, *state);
        const char* call = state->max_concurrent == 1 ? "_dispatch_sync_f" : "_dispatch_barrier_sync_f";
        static_cast<void>(bridge->call(security::libsystem_library, call, {queue, 0, noop_function}));
        // Operations waiting for dependencies or finishing asynchronously.
        while (state->pending.load() > 0) static_cast<void>(bridge->call(security::libsystem_library, "_usleep", {1000}));
    }, false, "v@:");
    b.method(cls, "maxConcurrentOperationCount", [require](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<Address>(require(s.x[0])->max_concurrent);
    }, false, "q@:");
    b.method(cls, "setMaxConcurrentOperationCount:", [require](CpuState& s, GuestMemory&) {
        const auto state = require(s.x[0]);
        std::lock_guard lock(state->mutex);
        const auto value = static_cast<std::int64_t>(s.x[2]);
        if (state->dispatch && (value == 1) != (state->max_concurrent == 1))
            security::unsupported("changing NSOperationQueue concurrency after operations were added");
        state->max_concurrent = value;
    }, false, "v@:q");
    b.method(cls, "name", [require](CpuState& s, GuestMemory&) { s.x[0] = require(s.x[0])->name; });
    b.method(cls, "setName:", [bridge, require](CpuState& s, GuestMemory&) {
        const auto state = require(s.x[0]);
        const auto copy = s.x[2] ? bridge->cf.copy(s.x[2]) : 0;
        std::lock_guard lock(state->mutex);
        bridge->release(std::exchange(state->name, copy));
    }, false, "v@:@");
    b.method(cls, "qualityOfService", [require](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(require(s.x[0])->quality); }, false, "q@:");
    b.method(cls, "setQualityOfService:", [require](CpuState& s, GuestMemory&) { require(s.x[0])->quality = static_cast<std::int64_t>(s.x[2]); }, false, "v@:q");
    b.method(cls, "underlyingQueue", [require](CpuState& s, GuestMemory&) {
        const auto state = require(s.x[0]);
        std::lock_guard lock(state->mutex);
        s.x[0] = state->dispatch;
    });
    b.method(cls, "setUnderlyingQueue:", [bridge, require](CpuState& s, GuestMemory&) {
        const auto state = require(s.x[0]);
        std::lock_guard lock(state->mutex);
        if (state->main) security::unsupported("setting the main operation queue's underlying queue");
        if (state->dispatch) security::unsupported("setting an underlying queue after operations were added");
        if (s.x[2]) static_cast<void>(bridge->call(security::libsystem_library, "_dispatch_retain", {s.x[2]}));
        state->dispatch = s.x[2];
    }, false, "v@:@");
    b.method(cls, "isSuspended", [require](CpuState& s, GuestMemory&) { s.x[0] = require(s.x[0])->suspended; }, false, "B@:");
    b.method(cls, "setSuspended:", [bridge, require](CpuState& s, GuestMemory&) {
        const auto state = require(s.x[0]);
        const bool value = (s.x[2] & 0xff) != 0;
        std::lock_guard lock(state->mutex);
        if (state->main) security::unsupported("suspending the main operation queue");
        if (value == state->suspended) return;
        state->suspended = value;
        if (state->dispatch)
            static_cast<void>(bridge->call(security::libsystem_library, value ? "_dispatch_suspend" : "_dispatch_resume", {state->dispatch}));
    }, false, "v@:B");
    install_operations(b, cls);
}

} // namespace alchemy::gui::network
