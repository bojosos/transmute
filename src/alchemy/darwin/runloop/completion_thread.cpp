#include "alchemy/darwin_threads.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/runloop.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>

// Completion thread (owner: A2): a long-lived guest pthread for host-originated
// callbacks into guest code (Metal completed/scheduled handlers, shared-event
// listeners). See include/alchemy/gui/runloop.hpp.
namespace alchemy::gui {
namespace {
constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
constexpr std::string_view entry_library = "alchemy.internal";
constexpr std::string_view entry_symbol = "__alchemy_runloop_completion_thread";
constexpr auto poll_period = std::chrono::milliseconds(10);

struct Item {
    std::function<void()> work;
    std::uint64_t block{};
    std::vector<std::uint64_t> arguments;
};

struct State {
    State(ProcessContext& context, std::string thread_name) : process(context), name(std::move(thread_name)) {}
    ProcessContext& process;
    std::string name;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Item> items;
    bool closing{}, finished{};
    std::uint64_t identity{};   // guest thread identity once running
};

// Thread argument -> state. The entry is one synthetic function per process.
struct Table {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<State>> states;
    std::uint64_t next{1};
};
Table& table() { static Table value; return value; }

bool stopping(ProcessContext& process) {
    return process.requested_exit().has_value() || engine_stop_requested(process.calling_engine());
}

std::uint64_t symbol(ProcessContext& process, std::string_view name) {
    const auto value = process.resolve(system_library, name);
    if (!value) throw std::runtime_error("Alchemy completion thread requires " + std::string(name));
    return value->address;
}

void run_item(State& state, const Item& item) {
    if (item.work) { item.work(); return; }
    auto& engine = state.process.calling_engine();
    // block->invoke(block, arguments...), then the reference passed to post_block.
    struct Release {
        ProcessContext& process; std::uint64_t block;
        ~Release() {
            try {
                if (stopping(process)) return; // A stopping process runs no more guest code.
                const std::array arguments{block};
                static_cast<void>(EngineAdapter(process.calling_engine()).call(symbol(process, "__Block_release"), arguments));
            } catch (...) {}
        }
    } release{state.process, item.block};
    CallRegs registers;
    registers.x[0] = item.block;
    for (std::size_t index = 0; index < item.arguments.size(); ++index) registers.x[index + 1] = item.arguments[index];
    static_cast<void>(EngineAdapter(engine).call_regs(engine.read64(item.block + 16), registers));
}

void thread_body(const std::shared_ptr<State>& state) {
    auto& process = state->process;
    {
        std::lock_guard lock(state->mutex);
        state->identity = process.current_thread()->identity;
    }
    struct Finish {
        State& state;
        ~Finish() {
            { std::lock_guard lock(state.mutex); state.finished = true; state.items.clear(); }
            state.changed.notify_all();
        }
    } finish{*state};
    // pthread_setname_np names the calling thread (Darwin signature).
    if (const auto setname = process.resolve(system_library, "_pthread_setname_np")) {
        const auto text = process.allocate_guest(state->name.size() + 1);
        struct Text { ProcessContext& process; std::uint64_t address; ~Text() { process.release_guest(address); } } owned{process, text};
        auto& memory = process.calling_engine();
        memory.write_bytes(text, {reinterpret_cast<const std::uint8_t*>(state->name.data()), state->name.size()});
        memory.fill_bytes(text + state->name.size(), 0, 1);
        const std::array arguments{text};
        static_cast<void>(EngineAdapter(memory).call(setname->address, arguments));
    }
    for (;;) {
        Item item;
        {
            std::unique_lock lock(state->mutex);
            while (state->items.empty() && !state->closing && !stopping(process)) state->changed.wait_for(lock, poll_period);
            if (stopping(process)) {
                // Pending work is dropped; the worker unwinds without guest cleanup.
                lock.unlock();
                if (guest_worker_must_stop(process)) throw GuestThreadStop{};
                return;
            }
            if (state->items.empty()) return; // shutdown() after the queue drained
            item = std::move(state->items.front());
            state->items.pop_front();
        }
        run_item(*state, item);
    }
}

class Thread final : public CompletionThread {
public:
    explicit Thread(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Thread() override {
        // Without shutdown() the thread drains its queue and ends on its own.
        { std::lock_guard lock(state_->mutex); state_->closing = true; }
        state_->changed.notify_all();
    }
    void post(std::function<void()> work) override {
        if (!work) throw std::invalid_argument("completion thread work is empty");
        enqueue({std::move(work), 0, {}});
    }
    void post_block(std::uint64_t block, std::vector<std::uint64_t> arguments) override {
        if (!block) throw std::invalid_argument("completion thread block is NULL");
        if (arguments.size() > 7) throw std::invalid_argument("completion thread blocks take at most 7 integer arguments");
        enqueue({{}, block, std::move(arguments)});
    }
    void shutdown() override {
        auto& process = state_->process;
        std::unique_lock lock(state_->mutex);
        if (state_->identity && process.current_thread()->identity == state_->identity)
            throw std::logic_error("a completion thread cannot shut itself down");
        state_->closing = true;
        state_->changed.notify_all();
        // A stopping process may never schedule the thread again.
        while (!state_->finished && !stopping(process)) state_->changed.wait_for(lock, poll_period);
    }

private:
    void enqueue(Item item) {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->closing || state_->finished) throw std::runtime_error("completion thread " + state_->name + " is shut down");
            state_->items.push_back(std::move(item));
        }
        state_->changed.notify_one();
    }
    std::shared_ptr<State> state_;
};
} // namespace

std::shared_ptr<CompletionThread> start_completion_thread(ProcessContext& process, std::string name) {
    // One synthetic entry per process, installed on first use.
    auto entry = process.resolve(entry_library, entry_symbol);
    if (!entry) {
        static_cast<void>(process.install_function(std::string(entry_symbol), [](auto& cpu, auto&) {
            std::shared_ptr<State> state;
            {
                std::lock_guard lock(table().mutex);
                const auto found = table().states.find(cpu.x[0]);
                if (found == table().states.end()) throw std::runtime_error("completion thread lost its state");
                state = found->second;
                table().states.erase(found);
            }
            thread_body(state);
            cpu.x[0] = 0;
        }, std::string(entry_library)));
        entry = process.resolve(entry_library, entry_symbol);
        if (!entry) throw std::runtime_error("completion thread entry was not installed");
    }
    auto state = std::make_shared<State>(process, std::move(name));
    std::uint64_t key;
    { std::lock_guard lock(table().mutex); key = table().next++; table().states.emplace(key, state); }
    const auto forget = [&] { std::lock_guard lock(table().mutex); table().states.erase(key); };
    auto& engine = process.calling_engine();
    const auto cell = process.allocate_guest(8);
    struct Cell { ProcessContext& process; std::uint64_t address; ~Cell() { process.release_guest(address); } } owned{process, cell};
    engine.write64(cell, 0);
    std::uint32_t result;
    try {
        const std::array arguments{cell, std::uint64_t{0}, entry->address, key};
        result = static_cast<std::uint32_t>(EngineAdapter(engine).call(symbol(process, "_pthread_create"), arguments));
    } catch (...) { forget(); throw; }
    if (result) {
        forget();
        throw std::runtime_error("completion thread " + state->name + " could not start (errno " + std::to_string(result) + ")");
    }
    const std::array detach{engine.read64(cell)};
    static_cast<void>(EngineAdapter(engine).call(symbol(process, "_pthread_detach"), detach));
    return std::make_shared<Thread>(std::move(state));
}

} // namespace alchemy::gui
