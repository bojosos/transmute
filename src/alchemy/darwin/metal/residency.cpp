// Residency sets (MTLResidencySet, macOS 15) and Metal IO (MTLIO, macOS 13)
// (owner: A5).
//
// Residency sets: Vulkan keeps every allocation of the device resident for
// its whole life, so what a residency set asks for always holds. The set is
// real bookkeeping: it keeps (retains) its allocations, answers their count
// and size, and the queue and command buffer calls that attach it check
// their arguments and need nothing more.
//
// Metal IO: not provided. The device's MTLIO factories answer nil with an
// MTLIOErrorDomain error, as Metal does for a handle it cannot open, so a
// client falls back to its own file reads.
#include "alchemy/darwin/metal/bridge.hpp"

#include <algorithm>
#include <memory>
#include <vector>

namespace alchemy::metal::bridge {
namespace {

// MTLIOError (MTLIOCommandQueue.h): MTLIOErrorURLInvalid = 1, MTLIOErrorInternal = 2.
constexpr std::int64_t io_error_internal = 2;
constexpr std::string_view io_unavailable = "Metal IO (MTLIO) is not provided by Alchemy's Metal on Vulkan";

// The allocations are Held::owned (+1 each, released by -dealloc).
struct ResidencySetState final : Held {
    Address device{};
    std::uint64_t committed_size{};
};

std::shared_ptr<ResidencySetState> residency_set(Address object) { return state<ResidencySetState>(object, "MTLResidencySet"); }

// The objects of an id array pointer and count; nil elements are errors.
std::vector<Address> objects_at(GuestMemory& memory, Address pointer, std::uint64_t count) {
    if (count && !pointer) throw std::runtime_error("Alchemy Metal: null object array");
    std::vector<Address> result;
    result.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto object = memory.read64(pointer + 8 * i);
        if (!object) throw std::runtime_error("Alchemy Metal: nil element in an MTLAllocation or MTLResidencySet array");
        result.push_back(object);
    }
    return result;
}

void add(ResidencySetState& set, const std::vector<Address>& allocations) {
    for (const auto allocation : allocations) {
        {
            std::lock_guard lock(set.mutex);
            if (std::find(set.owned.begin(), set.owned.end(), allocation) != set.owned.end()) continue;
            set.owned.push_back(allocation);
        }
        runtime().retain(allocation);
    }
}

void remove(ResidencySetState& set, const std::vector<Address>& allocations) {
    std::vector<Address> released;
    {
        std::lock_guard lock(set.mutex);
        for (const auto allocation : allocations) {
            const auto found = std::find(set.owned.begin(), set.owned.end(), allocation);
            if (found == set.owned.end()) continue;
            set.owned.erase(found);
            released.push_back(allocation);
        }
    }
    for (const auto allocation : released) runtime().release(allocation);
}

std::vector<Address> snapshot(ResidencySetState& set) {
    std::lock_guard lock(set.mutex);
    return set.owned;
}

} // namespace

void install_residency_and_io(Runtime& r) {
    const auto device = r.objc.find_class("AlchemyMTLDevice");
    const auto queue = r.objc.find_class("AlchemyMTLCommandQueue");
    const auto command_buffer = r.objc.find_class("AlchemyMTLCommandBuffer");
    if (!device || !queue || !command_buffer)
        throw std::runtime_error("Alchemy Metal: install_residency_and_io before the device, queue and command buffer classes");

    const auto set_class = r.define_class("AlchemyMTLResidencySet", "NSObject", false);
    r.objc.add_native_protocol(set_class, "MTLResidencySet");
    install_label(set_class);
    r.install_dealloc(set_class);
    r.method(set_class, "device", [](CpuState& s, GuestMemory&) { s.x[0] = residency_set(s.x[0])->device; });
    r.method(set_class, "addAllocation:", [](CpuState& s, GuestMemory&) {
        if (!s.x[2]) throw std::runtime_error("Alchemy Metal: -[MTLResidencySet addAllocation:] with nil");
        add(*residency_set(s.x[0]), {s.x[2]});
    }, false, "v@:@");
    r.method(set_class, "addAllocations:count:", [](CpuState& s, GuestMemory& m) {
        add(*residency_set(s.x[0]), objects_at(m, s.x[2], s.x[3]));
    }, false, "v@:^@Q");
    r.method(set_class, "removeAllocation:", [](CpuState& s, GuestMemory&) {
        remove(*residency_set(s.x[0]), {s.x[2]});
    }, false, "v@:@");
    r.method(set_class, "removeAllocations:count:", [](CpuState& s, GuestMemory& m) {
        remove(*residency_set(s.x[0]), objects_at(m, s.x[2], s.x[3]));
    }, false, "v@:^@Q");
    r.method(set_class, "removeAllAllocations", [](CpuState& s, GuestMemory&) {
        const auto set = residency_set(s.x[0]);
        remove(*set, snapshot(*set));
    }, false, "v@:");
    r.method(set_class, "containsAllocation:", [](CpuState& s, GuestMemory&) {
        const auto set = residency_set(s.x[0]);
        std::lock_guard lock(set->mutex);
        s.x[0] = std::find(set->owned.begin(), set->owned.end(), s.x[2]) != set->owned.end();
    }, false, "B@:@");
    r.method(set_class, "allocationCount", [](CpuState& s, GuestMemory&) {
        const auto set = residency_set(s.x[0]);
        std::lock_guard lock(set->mutex);
        s.x[0] = set->owned.size();
    }, false, "Q@:");
    r.method(set_class, "allAllocations", [&r](CpuState& s, GuestMemory&) {
        s.x[0] = r.autorelease(r.cf.array(snapshot(*residency_set(s.x[0]))));
    }, false, "@@:");
    // allocatedSize: the memory of the allocations as of the last commit.
    r.method(set_class, "allocatedSize", [](CpuState& s, GuestMemory&) {
        const auto set = residency_set(s.x[0]);
        std::lock_guard lock(set->mutex);
        s.x[0] = set->committed_size;
    }, false, "Q@:");
    r.method(set_class, "commit", [&r](CpuState& s, GuestMemory&) {
        const auto set = residency_set(s.x[0]);
        std::uint64_t size = 0;
        for (const auto allocation : snapshot(*set)) size += r.objc.send(allocation, "allocatedSize").x0;
        std::lock_guard lock(set->mutex);
        set->committed_size = size;
    }, false, "v@:");
    // Everything is resident already: residency requests have nothing to do.
    for (const auto selector : {"requestResidency", "endResidency"})
        r.method(set_class, selector, [](CpuState& s, GuestMemory&) { static_cast<void>(residency_set(s.x[0])); }, false, "v@:");

    // -[MTLDevice newResidencySetWithDescriptor:error:] (+1); the descriptor
    // class is in the descriptor table (metal_descriptors.cpp).
    r.method(device, "newResidencySetWithDescriptor:error:", [&r, set_class](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<ResidencySetState>();
        value->device = s.x[0];
        if (s.x[2])
            if (const auto label = descriptor_object(s.x[2], "label")) value->label = r.objc.send(label, "copy").x0;
        s.x[0] = r.create(set_class, std::move(value));
    }, false, "@@:@^@");

    // Attaching sets to a queue or a command buffer: checked, nothing else to do.
    const auto check_one = [](CpuState& s, GuestMemory&) { static_cast<void>(residency_set(s.x[2])); };
    const auto check_many = [](CpuState& s, GuestMemory& m) {
        for (const auto set : objects_at(m, s.x[2], s.x[3])) static_cast<void>(residency_set(set));
    };
    for (const auto selector : {"addResidencySet:", "removeResidencySet:"}) r.method(queue, selector, check_one, false, "v@:@");
    for (const auto selector : {"addResidencySets:count:", "removeResidencySets:count:"})
        r.method(queue, selector, check_many, false, "v@:^@Q");
    r.method(command_buffer, "useResidencySet:", check_one, false, "v@:@");
    r.method(command_buffer, "useResidencySets:count:", check_many, false, "v@:^@Q");

    // Metal IO: the factories fail with MTLIOErrorDomain / MTLIOErrorInternal.
    for (const auto& [selector, error_index] : std::initializer_list<std::pair<const char*, unsigned>>{
             {"newIOCommandQueueWithDescriptor:error:", 3}, {"newIOHandleWithURL:error:", 3},
             {"newIOFileHandleWithURL:error:", 3}, {"newIOHandleWithURL:compressionMethod:error:", 4},
             {"newIOFileHandleWithURL:compressionMethod:error:", 4}}) {
        const std::string types = error_index == 3 ? "@@:@^@" : "@@:@q^@";
        r.method(device, selector, [&r, error_index](CpuState& s, GuestMemory& m) {
            r.set_error(m, s.x[error_index], "MTLIOErrorDomain", io_error_internal, io_unavailable);
            s.x[0] = 0;
        }, false, types);
    }
}

} // namespace alchemy::metal::bridge
