#pragma once

#include "transmute/a64.hpp"
#include "transmute/jit.hpp"
#include "execution_gate.hpp"

namespace transmute {

// This private context owns the backing lifetime for one synchronous native
// invocation chain. Shared mode admits compiled register bodies and naturally
// aligned scalar memory regions. Decode, checked effects and atomics remain
// exclusive except guarded native single-element LSE; shared write bookkeeping
// commits under the short metadata lock. LL/SC and pair atomics stay exclusive.
class NativeMemoryContext final {
private:
    friend class ReferenceEngine;
    NativeMemoryContext(ReferenceEngine& engine, bool allow_shared)
        : engine_(engine), gate_(*engine.address_space_->execution_gate),
          mutex_(engine.address_space_->mutex, std::defer_lock) {
        if (allow_shared && !engine.force_exclusive_jit_ &&
            engine.jit_enabled_ && engine.jit_cache_ &&
            !ExecutionGate::Exclusive::held(gate_) &&
            engine.jit_cache_->has_parallel_cached_block(
                engine.state_.pc, engine.jit_seen_generation_)) {
            // A known reader must not join the exclusive queue just because
            // a writer is pending: readers would perpetuate that queue after
            // the writer finishes. Retry shared admission, then validate the
            // address-space generation only while the shared gate is held.
            for (;;) {
                reader_.emplace(gate_);
                if (reader_->owns_lock()) break;
                reader_.reset();
                gate_.wait_for_pending_writer();
            }
            if (reader_->owns_lock() && engine.jit_cache_->has_parallel_cached_block(
                    engine.state_.pc, engine.address_space_->code_generation)) {
                if (engine.jit_profiling_) {
                    auto& space = *engine.address_space_;
                    const auto concurrent = space.profiled_shared_readers.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                    auto peak = space.peak_profiled_shared_readers.load(std::memory_order_relaxed);
                    while (peak < concurrent && !space.peak_profiled_shared_readers.
                        compare_exchange_weak(peak, concurrent, std::memory_order_relaxed)) {}
                    space.profiled_shared_read_leases.fetch_add(1, std::memory_order_relaxed);
                    profiled_read_ = true;
                }
                return;
            }
            reader_.reset();
        }
        promote_exclusive();
    }

public:
    ~NativeMemoryContext() { release_profile(); }
    NativeMemoryContext(const NativeMemoryContext&) = delete;
    NativeMemoryContext& operator=(const NativeMemoryContext&) = delete;
    NativeMemoryContext(NativeMemoryContext&&) = delete;
    NativeMemoryContext& operator=(NativeMemoryContext&&) = delete;

    [[nodiscard]] bool owns_lock() const noexcept {
        return is_exclusive() || (reader_ && reader_->owns_lock());
    }
    [[nodiscard]] bool is_exclusive() const noexcept {
        return exclusive_.has_value() && mutex_.owns_lock();
    }
    [[nodiscard]] bool permits_cached_region(std::uint64_t pc,
                                             std::uint64_t generation) const noexcept {
        assert(owns_lock());
        return is_exclusive() ||
            (!engine_.force_exclusive_jit_ && !gate_.has_pending_writer() &&
             engine_.jit_cache_->has_parallel_cached_block(pc, generation));
    }
    [[nodiscard]] std::size_t maximum_chain_steps() const noexcept {
        return is_exclusive() ? 256 : 8192;
    }
    [[nodiscard]] std::size_t maximum_chain_blocks() const noexcept {
        return is_exclusive() ? 32 : 512;
    }
    void promote_exclusive() {
        if (is_exclusive()) {
            engine_.force_exclusive_jit_ = false;
            return;
        }
        // No generated frame or borrowed pointer may cross this transition.
        release_profile();
        reader_.reset();
        exclusive_.emplace(gate_);
        mutex_.lock();
        engine_.force_exclusive_jit_ = false;
    }
    void unlock() {
        release_profile();
        if (mutex_.owns_lock()) mutex_.unlock();
        exclusive_.reset();
        reader_.reset();
    }

private:
    void release_profile() noexcept {
        if (!profiled_read_) return;
        engine_.address_space_->profiled_shared_readers.fetch_sub(1, std::memory_order_relaxed);
        profiled_read_ = false;
    }
    ReferenceEngine& engine_;
    ExecutionGate& gate_;
    std::optional<ExecutionGate::Exclusive> exclusive_;
    std::optional<ExecutionGate::Read> reader_;
    // Destroy the address-space lock before releasing the execution gate.
    std::unique_lock<std::recursive_mutex> mutex_;
    bool profiled_read_{};
};

} // namespace transmute
