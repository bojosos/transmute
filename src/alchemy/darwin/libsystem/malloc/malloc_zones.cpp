#include "alchemy/darwin/libsystem/malloc/malloc_zones.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace alchemy::libsystem {
namespace {

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr std::uint32_t enomem = 12, einval = 22;
constexpr std::uint64_t page_size = 16384;
constexpr std::uint32_t kern_success = 0, kern_invalid_argument = 4;

namespace layout = zone_layout;

class Zones {
public:
    Zones(ProcessContext& context, HostHeap heap, std::uint64_t system_zone)
        : context_(context), heap_(std::move(heap)), system_(system_zone), default_(system_zone) {
        zones_ = std::make_shared<const std::vector<std::uint64_t>>(1, system_zone);
    }

    [[nodiscard]] std::uint64_t system() const noexcept { return system_; }
    [[nodiscard]] bool host_owns(std::uint64_t address) const { return heap_.owns(address); }
    // Zone 0 (the system zone while no zone is registered at all).
    [[nodiscard]] std::uint64_t default_zone() const noexcept { return default_.load(std::memory_order_acquire); }
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint64_t>> zones() const {
        std::lock_guard lock(mutex_);
        return zones_;
    }

    // libmalloc order: register appends; unregister moves the last zone into
    // the freed slot. A zone already registered is not added twice.
    void register_zone(std::uint64_t zone) {
        std::lock_guard lock(mutex_);
        if (std::find(zones_->begin(), zones_->end(), zone) != zones_->end()) return;
        auto next = std::make_shared<std::vector<std::uint64_t>>(*zones_);
        next->push_back(zone);
        publish(std::move(next));
    }
    bool unregister_zone(std::uint64_t zone) {
        std::lock_guard lock(mutex_);
        const auto found = std::find(zones_->begin(), zones_->end(), zone);
        if (found == zones_->end()) return false;
        auto next = std::make_shared<std::vector<std::uint64_t>>(*zones_);
        const auto index = static_cast<std::size_t>(found - zones_->begin());
        (*next)[index] = next->back();
        next->pop_back();
        publish(std::move(next));
        return true;
    }

    // ---- guest calls into a zone's function table ----
    std::uint64_t call(std::uint64_t function, std::span<const std::uint64_t> arguments) {
        return context_.calling_engine().invoke_callback(function, arguments);
    }
    std::uint64_t slot(std::uint64_t zone, std::uint64_t offset) { return context_.calling_engine().read64(zone + offset); }
    std::uint32_t version(std::uint64_t zone) { return context_.calling_engine().read32(zone + layout::version); }

    // The zone that owns `address`: the system zone, or the first guest zone
    // whose size callback claims it (libmalloc's find_registered_zone); 0 if none.
    std::uint64_t owner(std::uint64_t address, std::uint64_t* size = nullptr) {
        if (!address) return 0;
        if (heap_.owns(address)) {
            if (size) *size = heap_.size(address);
            return system_;
        }
        if (!guest_zones_.load(std::memory_order_acquire)) return 0;
        const auto list = zones();
        for (const auto zone : *list) {
            if (zone == system_) continue;
            const auto function = slot(zone, layout::size);
            if (!function) continue;
            const std::array<std::uint64_t, 2> arguments{zone, address};
            if (const auto bytes = call(function, arguments)) {
                if (size) *size = bytes;
                return zone;
            }
        }
        return 0;
    }

    // ---- operations on one zone (the system zone stays on the host heap) ----
    std::uint64_t zone_malloc(std::uint64_t zone, std::uint64_t size) {
        if (zone == system_) return heap_.allocate(size, 16);
        return guest(zone, layout::malloc, {zone, size});
    }
    std::uint64_t zone_calloc(std::uint64_t zone, std::uint64_t count, std::uint64_t size) {
        if (zone == system_) {
            if (count && size > UINT64_MAX / count) { context_.set_errno(enomem); return 0; }
            return heap_.allocate(count * size, 16);   // zero-filled
        }
        return guest(zone, layout::calloc, {zone, count, size});
    }
    std::uint64_t zone_valloc(std::uint64_t zone, std::uint64_t size) {
        if (zone == system_) return heap_.allocate(size, page_size);
        return guest(zone, layout::valloc, {zone, size});
    }
    std::uint64_t zone_memalign(std::uint64_t zone, std::uint64_t alignment, std::uint64_t size) {
        if (alignment < 8 || (alignment & (alignment - 1))) { context_.set_errno(einval); return 0; }
        if (zone == system_) return heap_.allocate(size, alignment);
        // memalign exists from version 5 on; an older zone cannot align.
        if (version(zone) < 5) { context_.set_errno(enomem); return 0; }
        return guest(zone, layout::memalign, {zone, alignment, size});
    }
    void zone_free(std::uint64_t zone, std::uint64_t address) {
        if (!address) return;
        if (zone == system_) { heap_.release(address); return; }
        static_cast<void>(guest(zone, layout::free, {zone, address}));
    }
    std::uint64_t zone_realloc(std::uint64_t zone, std::uint64_t address, std::uint64_t size) {
        if (zone != system_) return guest(zone, layout::realloc, {zone, address, size});
        if (!address) return heap_.allocate(size, 16);
        if (!size) { heap_.release(address); return 0; }
        if (!heap_.owns(address)) throw std::runtime_error("Darwin realloc received an invalid allocation");
        const auto old_size = heap_.size(address);
        const auto result = heap_.allocate(size, 16);
        if (!result) return 0;   // the old block stays valid
        context_.calling_engine().copy_bytes(result, address, std::min(old_size, size));
        heap_.release(address);
        return result;
    }
    // malloc_statistics_t {blocks_in_use, size_in_use, max_size_in_use, size_allocated}.
    std::array<std::uint64_t, 4> zone_statistics(std::uint64_t zone) {
        std::array<std::uint64_t, 4> result{};
        if (zone == system_) {
            heap_.statistics(result[0], result[1], result[2]);
            result[3] = result[1];
            return result;
        }
        const auto introspect = slot(zone, layout::introspect);
        const auto function = introspect ? context_.calling_engine().read64(introspect + layout::introspect_statistics) : 0;
        if (!function) return result;
        const auto buffer = heap_.allocate(32, 16);
        if (!buffer) throw std::bad_alloc();
        const std::array<std::uint64_t, 2> arguments{zone, buffer};
        try { static_cast<void>(call(function, arguments)); }
        catch (...) { heap_.release(buffer); throw; }
        auto& memory = context_.calling_engine();
        result = {memory.read32(buffer), memory.read64(buffer + 8), memory.read64(buffer + 16), memory.read64(buffer + 24)};
        heap_.release(buffer);
        return result;
    }

    // ---- process-wide calls (malloc(3) and friends) ----
    std::uint64_t free_pointer(std::uint64_t address) {
        if (!address) return 0;
        if (heap_.owns(address)) { heap_.release(address); return 0; }
        const auto zone = owner(address);
        if (!zone) throw std::runtime_error("Darwin free received an invalid allocation");
        zone_free(zone, address);
        return 0;
    }
    std::uint64_t realloc_pointer(std::uint64_t address, std::uint64_t size) {
        if (!address) return zone_malloc(default_zone(), size);
        const auto zone = owner(address);
        if (!zone) throw std::runtime_error("Darwin realloc received an invalid allocation");
        return zone_realloc(zone, address, size);
    }
    std::uint64_t size_of(std::uint64_t address) {
        std::uint64_t size{};
        return owner(address, &size) ? size : 0;
    }

    // malloc_get_all_zones: a guest array libmalloc owns (rewritten here on every call).
    std::pair<std::uint64_t, std::uint32_t> all_zones() {
        const auto list = zones();
        std::lock_guard lock(array_mutex_);
        if (array_capacity_ < list->size()) {
            const auto capacity = std::max<std::size_t>(8, list->size() * 2);
            array_ = context_.allocate_guest(capacity * 8);   // an earlier array stays valid for its readers
            array_capacity_ = capacity;
        }
        auto& memory = context_.calling_engine();
        for (std::size_t index = 0; index < list->size(); ++index) memory.write64(array_ + index * 8, (*list)[index]);
        return {array_, static_cast<std::uint32_t>(list->size())};
    }

private:
    std::uint64_t guest(std::uint64_t zone, std::uint64_t offset, std::initializer_list<std::uint64_t> arguments) {
        const auto function = slot(zone, offset);
        if (!function) return 0;
        return call(function, std::span<const std::uint64_t>(arguments.begin(), arguments.size()));
    }
    void publish(std::shared_ptr<std::vector<std::uint64_t>> next) {
        default_.store(next->empty() ? system_ : next->front(), std::memory_order_release);
        guest_zones_.store(std::any_of(next->begin(), next->end(), [this](auto zone) { return zone != system_; }),
                           std::memory_order_release);
        zones_ = std::move(next);
    }

    ProcessContext& context_;
    HostHeap heap_;
    const std::uint64_t system_;
    std::atomic<std::uint64_t> default_;
    std::atomic<bool> guest_zones_{};
    mutable std::mutex mutex_;
    std::shared_ptr<const std::vector<std::uint64_t>> zones_;
    std::mutex array_mutex_;
    std::uint64_t array_{};
    std::size_t array_capacity_{};
};

// A zone argument of NULL names the default zone.
std::uint64_t zone_argument(Zones& zones, std::uint64_t zone) { return zone ? zone : zones.default_zone(); }

// malloc_good_size's rounding: the 16-byte quantum, at least one quantum.
std::uint64_t good_size(std::uint64_t size) { return size > UINT64_MAX - 15 ? size : std::max<std::uint64_t>(16, (size + 15) & ~15ULL); }

// The system zone's members DarwinServices leaves to the registry (destroy,
// batch_malloc, batch_free, introspect, pressure_relief, claimed_address)
// and its malloc_introspection_t. Allocator shims (Chromium's
// allocator_interception_apple, design) copy and call these on the default
// and purgeable zones, so none is NULL up to the advertised version.
void complete_system_zone(ProcessContext& context, const std::shared_ptr<Zones>& zones) {
    auto* process = &context;
    using Body = std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)>;
    const auto function = [process](const char* name, Body body) {
        return process->install_function(name, [body = std::move(body)](auto& s, auto& m) { s.x[0] = body(s, m); }, "alchemy.internal");
    };
    const auto system = zones->system();
    auto& memory = context.calling_engine();
    const auto put = [&](std::uint64_t offset, std::uint64_t value) { memory.write64(system + offset, value); };
    const Body zero = [](auto&, auto&) { return std::uint64_t{0}; };

    // The system zone is the process heap: destroying it is a guest error with no Darwin result.
    put(layout::destroy, function("__alchemy_zone_destroy", [](auto&, auto&) -> std::uint64_t {
        throw std::runtime_error("Darwin malloc_destroy_zone: the default malloc zone cannot be destroyed");
    }));
    // batch_malloc(zone, size, void **results, unsigned count) -> allocated; batch_free(zone, void **, count).
    put(layout::batch_malloc, function("__alchemy_zone_batch_malloc", [zones](auto& s, auto& m) {
        std::uint32_t done = 0;
        for (; done < static_cast<std::uint32_t>(s.x[3]); ++done) {
            const auto address = zones->zone_malloc(zones->system(), s.x[1]);
            if (!address) break;
            m.write64(s.x[2] + done * 8ULL, address);
        }
        return std::uint64_t{done};
    }));
    put(layout::batch_free, function("__alchemy_zone_batch_free", [zones](auto& s, auto& m) {
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(s.x[2]); ++index)
            zones->zone_free(zones->system(), m.read64(s.x[1] + index * 8ULL));
        return std::uint64_t{0};
    }));
    // pressure_relief(zone, goal) -> bytes released: the host heap keeps no free memory to return.
    put(layout::pressure_relief, function("__alchemy_zone_pressure_relief", zero));
    // claimed_address(zone, ptr): blocks of this zone (their start addresses).
    put(layout::claimed_address, function("__alchemy_zone_claimed_address", [zones](auto& s, auto&) {
        return std::uint64_t{zones->host_owns(s.x[1])};
    }));

    namespace in = layout::introspection;
    const auto table = context.allocate_guest(in::size);
    memory.fill_bytes(table, 0, in::size);
    const auto entry = [&](std::uint64_t offset, std::uint64_t value) { memory.write64(table + offset, value); };
    // enumerator(task, context, type_mask, zone, reader, recorder): the host heap
    // is not enumerable through a memory reader (KERN_NOT_SUPPORTED).
    entry(in::enumerator, function("__alchemy_zone_enumerator", [](auto&, auto&) { return std::uint64_t{46}; }));
    entry(in::good_size, function("__alchemy_zone_good_size", [](auto& s, auto&) { return good_size(s.x[1]); }));
    // check(zone) -> true: the host heap validates every free and realloc itself.
    entry(in::check, function("__alchemy_zone_check", [](auto&, auto&) { return std::uint64_t{1}; }));
    entry(in::print, function("__alchemy_zone_print", zero));
    entry(in::log, function("__alchemy_zone_log", zero));
    // force_lock/force_unlock bracket fork(): the host heap holds its locks only
    // inside one host call, never across guest code, so there is nothing to hold.
    entry(in::force_lock, function("__alchemy_zone_force_lock", zero));
    entry(in::force_unlock, function("__alchemy_zone_force_unlock", zero));
    entry(in::statistics, function("__alchemy_zone_statistics", [zones](auto& s, auto& m) {
        const auto value = zones->zone_statistics(zones->system());
        m.write32(s.x[1], static_cast<std::uint32_t>(std::min<std::uint64_t>(value[0], UINT32_MAX)));
        m.write32(s.x[1] + 4, 0);
        m.write64(s.x[1] + 8, value[1]); m.write64(s.x[1] + 16, value[2]); m.write64(s.x[1] + 24, value[3]);
        return std::uint64_t{0};
    }));
    entry(in::zone_locked, function("__alchemy_zone_locked", zero));   // never locked between calls
    // Discharge checking (version 7) is not available: enable answers false.
    entry(in::enable_discharge_checking, function("__alchemy_zone_enable_discharge_checking", zero));
    entry(in::disable_discharge_checking, function("__alchemy_zone_disable_discharge_checking", zero));
    entry(in::discharge, function("__alchemy_zone_discharge", zero));
    // enumerate_discharged_pointers(zone, block) reports discharged memory only: none exists.
    entry(in::enumerate_discharged_pointers, function("__alchemy_zone_enumerate_discharged_pointers", zero));
    entry(in::reinit_lock, function("__alchemy_zone_reinit_lock", zero));
    entry(in::print_task, function("__alchemy_zone_print_task", zero));
    // task_statistics(task, zone, reader, stats): the system zone of this task only.
    entry(in::task_statistics, function("__alchemy_zone_task_statistics", [zones, process](auto& s, auto& m) -> std::uint64_t {
        std::uint64_t self{};
        if (const auto cell = process->resolve(libsystem, "_mach_task_self_")) self = m.read32(cell->address);
        if ((s.x[0] && static_cast<std::uint32_t>(s.x[0]) != self) || s.x[1] != zones->system() || !s.x[3]) return 5;   // KERN_FAILURE
        const auto value = zones->zone_statistics(zones->system());
        m.write32(s.x[3], static_cast<std::uint32_t>(std::min<std::uint64_t>(value[0], UINT32_MAX)));
        m.write32(s.x[3] + 4, 0);
        m.write64(s.x[3] + 8, value[1]); m.write64(s.x[3] + 16, value[2]); m.write64(s.x[3] + 24, value[3]);
        return 0;
    }));
    // zone_type 0: MALLOC_ZONE_TYPE_UNKNOWN (value from the open-source libmalloc, unverified).
    memory.write32(table + in::zone_type, 0);
    put(layout::introspect, table);
    memory.write32(system + layout::version, layout::system_version);
}

} // namespace

void install_malloc_zones(ProcessContext& context, HostHeap heap, std::uint64_t system_zone) {
    auto zones = std::make_shared<Zones>(context, std::move(heap), system_zone);
    complete_system_zone(context, zones);
    using Operation = std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)>;
    // A failed allocation returns 0 with errno ENOMEM.
    auto* process = &context;
    const auto install = [process](const char* name, Operation operation) {
        static_cast<void>(process->install_function(name, [process, operation = std::move(operation)](auto& s, auto& m) {
            try { s.x[0] = operation(s, m); }
            catch (const std::bad_alloc&) { process->set_errno(enomem); s.x[0] = 0; }
        }));
    };

    // malloc(3): allocation goes to the default zone; free, realloc and
    // malloc_size to the zone that owns the pointer.
    install("_malloc", [zones](auto& s, auto&) { return zones->zone_malloc(zones->default_zone(), s.x[0]); });
    install("_calloc", [zones](auto& s, auto&) { return zones->zone_calloc(zones->default_zone(), s.x[0], s.x[1]); });
    install("_valloc", [zones](auto& s, auto&) { return zones->zone_valloc(zones->default_zone(), s.x[0]); });
    install("_free", [zones](auto& s, auto&) { return zones->free_pointer(s.x[0]); });
    install("_realloc", [zones](auto& s, auto&) { return zones->realloc_pointer(s.x[0], s.x[1]); });
    // reallocf frees the old block when the reallocation fails.
    install("_reallocf", [zones](auto& s, auto&) {
        const auto result = zones->realloc_pointer(s.x[0], s.x[1]);
        if (!result && s.x[1]) static_cast<void>(zones->free_pointer(s.x[0]));
        return result;
    });
    install("_malloc_size", [zones](auto& s, auto&) { return zones->size_of(s.x[0]); });
    // posix_memalign returns the error instead of setting errno.
    install("_posix_memalign", [zones, process](auto& s, auto& m) -> std::uint64_t {
        const auto alignment = s.x[1];
        if (alignment < 8 || (alignment & (alignment - 1))) return einval;
        const auto address = zones->zone_memalign(zones->default_zone(), alignment, s.x[2]);
        if (!address) { process->set_errno(enomem); return enomem; }
        m.write64(s.x[0], address);
        return 0;
    });
    install("_aligned_alloc", [zones, process](auto& s, auto&) -> std::uint64_t {
        if (s.x[0] < 8 || (s.x[0] & (s.x[0] - 1)) || s.x[1] % s.x[0]) { process->set_errno(einval); return 0; }
        return zones->zone_memalign(zones->default_zone(), s.x[0], s.x[1]);
    });

    // ---- the zone registry ----
    install("_malloc_default_zone", [zones](auto&, auto&) { return zones->default_zone(); });
    // The purgeable zone is the system zone: Alchemy has no purgeable memory.
    install("_malloc_default_purgeable_zone", [zones](auto&, auto&) { return zones->system(); });
    install("_malloc_zone_register", [zones](auto& s, auto&) {
        if (s.x[0]) zones->register_zone(s.x[0]);
        return std::uint64_t{0};
    });
    install("_malloc_zone_unregister", [zones](auto& s, auto&) {
        if (!zones->unregister_zone(s.x[0]))
            std::fprintf(stderr, "alchemy: malloc_zone_unregister() failed for 0x%llx\n", static_cast<unsigned long long>(s.x[0]));
        return std::uint64_t{0};
    });
    // (task, memory_reader_t, vm_address_t **addresses, unsigned *count): this task only.
    install("_malloc_get_all_zones", [zones, process](auto& s, auto& m) -> std::uint64_t {
        std::uint64_t self{};
        if (const auto cell = process->resolve(libsystem, "_mach_task_self_")) self = m.read32(cell->address);
        if (s.x[0] && static_cast<std::uint32_t>(s.x[0]) != self) return kern_invalid_argument;
        const auto [array, count] = zones->all_zones();
        if (s.x[2]) m.write64(s.x[2], array);
        if (s.x[3]) m.write32(s.x[3], count);
        return kern_success;
    });
    install("_malloc_zone_from_ptr", [zones](auto& s, auto&) { return zones->owner(s.x[0]); });
    // One shared host heap: a created zone is the system zone, and destroying it frees nothing.
    install("_malloc_create_zone", [zones](auto&, auto&) { return zones->system(); });
    install("_malloc_destroy_zone", [zones](auto& s, auto&) {
        if (s.x[0] && s.x[0] != zones->system()) {
            static_cast<void>(zones->unregister_zone(s.x[0]));
            if (const auto destroy = zones->slot(s.x[0], zone_layout::destroy)) {
                const std::array<std::uint64_t, 1> arguments{s.x[0]};
                static_cast<void>(zones->call(destroy, arguments));
            }
        }
        return std::uint64_t{0};
    });
    install("_malloc_get_zone_name", [zones](auto& s, auto& m) {
        const auto zone = zone_argument(*zones, s.x[0]);
        return m.read64(zone + zone_layout::zone_name);
    });
    // A guest zone gets a heap copy of the name; the system zone keeps its own.
    install("_malloc_set_zone_name", [zones](auto& s, auto& m) {
        if (!s.x[0] || s.x[0] == zones->system() || !s.x[1]) return std::uint64_t{0};
        bool terminated{};
        const auto name = m.read_string(s.x[1], 1, 4096, &terminated);
        if (!terminated) throw std::runtime_error("malloc_set_zone_name: name exceeds 4 KiB");
        const auto copy = zones->zone_malloc(zones->system(), name.size() + 1);
        if (!copy) return std::uint64_t{0};
        m.write_bytes(copy, name);
        m.write8(copy + name.size(), 0);
        m.write64(s.x[0] + zone_layout::zone_name, copy);
        return std::uint64_t{0};
    });

    // ---- malloc_zone_* calls on one zone ----
    install("_malloc_zone_malloc", [zones](auto& s, auto&) { return zones->zone_malloc(zone_argument(*zones, s.x[0]), s.x[1]); });
    install("_malloc_zone_calloc", [zones](auto& s, auto&) {
        return zones->zone_calloc(zone_argument(*zones, s.x[0]), s.x[1], s.x[2]);
    });
    install("_malloc_zone_valloc", [zones](auto& s, auto&) { return zones->zone_valloc(zone_argument(*zones, s.x[0]), s.x[1]); });
    install("_malloc_zone_memalign", [zones](auto& s, auto&) {
        return zones->zone_memalign(zone_argument(*zones, s.x[0]), s.x[1], s.x[2]);
    });
    install("_malloc_zone_free", [zones](auto& s, auto&) {
        zones->zone_free(zone_argument(*zones, s.x[0]), s.x[1]);
        return std::uint64_t{0};
    });
    install("_malloc_zone_realloc", [zones](auto& s, auto&) {
        return zones->zone_realloc(zone_argument(*zones, s.x[0]), s.x[1], s.x[2]);
    });
    // (zone, size, void **results, unsigned count) -> number allocated.
    install("_malloc_zone_batch_malloc", [zones](auto& s, auto& m) {
        const auto zone = zone_argument(*zones, s.x[0]);
        const auto count = static_cast<std::uint32_t>(s.x[3]);
        std::uint32_t done = 0;
        for (; done < count; ++done) {
            const auto address = zones->zone_malloc(zone, s.x[1]);
            if (!address) break;
            m.write64(s.x[2] + done * 8ULL, address);
        }
        return std::uint64_t{done};
    });
    install("_malloc_zone_batch_free", [zones](auto& s, auto& m) {
        const auto zone = zone_argument(*zones, s.x[0]);
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(s.x[2]); ++index)
            zones->zone_free(zone, m.read64(s.x[1] + index * 8ULL));
        return std::uint64_t{0};
    });
    // NULL: every zone. The system zone keeps no reclaimable memory.
    install("_malloc_zone_pressure_relief", [zones](auto& s, auto&) {
        std::uint64_t released = 0;
        const auto relieve = [&](std::uint64_t zone) {
            if (zone == zones->system() || zones->version(zone) < 8) return;
            if (const auto function = zones->slot(zone, zone_layout::pressure_relief)) {
                const std::array<std::uint64_t, 2> arguments{zone, s.x[1]};
                released += zones->call(function, arguments);
            }
        };
        if (s.x[0]) relieve(s.x[0]);
        else { const auto list = zones->zones(); for (const auto zone : *list) relieve(zone); }
        return released;
    });
    // (zone, malloc_statistics_t *): NULL sums every registered zone.
    install("_malloc_zone_statistics", [zones](auto& s, auto& m) {
        std::array<std::uint64_t, 4> total{};
        const auto add = [&](std::uint64_t zone) {
            const auto value = zones->zone_statistics(zone);
            for (std::size_t index = 0; index < total.size(); ++index) total[index] += value[index];
        };
        if (s.x[0]) add(s.x[0]);
        else { const auto list = zones->zones(); for (const auto zone : *list) add(zone); }
        m.write32(s.x[1], static_cast<std::uint32_t>(std::min<std::uint64_t>(total[0], UINT32_MAX)));
        m.write32(s.x[1] + 4, 0);
        m.write64(s.x[1] + 8, total[1]);
        m.write64(s.x[1] + 16, total[2]);
        m.write64(s.x[1] + 24, total[3]);
        return std::uint64_t{0};
    });
    // struct mstats {bytes_total, chunks_used, bytes_used, chunks_free, bytes_free}, returned in memory (x8).
    static_cast<void>(context.install_function("_mstats", [zones](auto& s, auto& m) {
        std::array<std::uint64_t, 4> total{};
        const auto list = zones->zones();
        for (const auto zone : *list) {
            const auto value = zones->zone_statistics(zone);
            for (std::size_t index = 0; index < total.size(); ++index) total[index] += value[index];
        }
        const std::array<std::uint64_t, 5> result{total[3], total[0], total[1], 0, total[3] - std::min(total[3], total[1])};
        for (std::size_t index = 0; index < result.size(); ++index) m.write64(s.x[8] + index * 8, result[index]);
    }));
}

} // namespace alchemy::libsystem
