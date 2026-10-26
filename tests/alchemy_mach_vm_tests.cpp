#include "alchemy/darwin_mach_vm.hpp"
#include "alchemy/process_context.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
constexpr std::uint64_t data = 0x500000000ULL;
constexpr std::uint64_t page_size = 16384;
constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext context{engine, [](auto, auto) -> alchemy::OutputResult {
        throw std::runtime_error("Mach VM test produced output");
    }};
    std::uint32_t task{};
    Fixture() {
        engine.map_memory(data, page_size);
        alchemy::install_darwin_mach_vm(context);
        const auto exported = context.resolve(system_library, "_mach_task_self_");
        require(exported && exported->kind == alchemy::ExportKind::data,
                "mach_task_self_ was not exported as guest data");
        task = engine.read32(exported->address);
        require(task != 0, "self-task identity is null");
    }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        const auto exported = context.resolve(system_library, name);
        require(exported.has_value(), "missing Mach VM export");
        return engine.invoke_callback(exported->address,
            std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t allocate(std::uint64_t size) {
        require(call("_vm_allocate", {task, data, size, 1}) == 0, "Mach allocation failed");
        return engine.read64(data);
    }
    void remap_outputs(std::uint64_t current, std::uint64_t maximum, unsigned inherit = 0) {
        const auto stack = (engine.state().sp - 256) & ~15ULL;
        engine.write64(stack, current);
        engine.write64(stack + 8, maximum);
        engine.write32(stack + 16, inherit);
    }
};

void allocations_and_protections() {
    Fixture f;
    require(f.call("_vm_allocate", {0, data, page_size, 1}) == 16, "invalid Mach task was accepted");
    require(f.call("_vm_allocate", {f.task, 0, page_size, 1}) == 1, "invalid address output was accepted");
    require(f.call("_vm_allocate", {f.task, data, page_size, 8}) == 46, "unsupported flags were accepted");
    const auto oversized = f.call("_vm_allocate", {f.task, data, 65ULL * 1024 * 1024, 1});
    require(oversized == 6, ("bounded allocation returned " + std::to_string(oversized) +
            " instead of KERN_RESOURCE_SHORTAGE").c_str());
    const auto address = f.allocate(page_size + 1);
    require(address % page_size == 0, "Mach allocation was not page aligned");
    const auto fresh = f.engine.query_region(address);
    require(fresh && fresh->readable && fresh->writable && !fresh->executable,
            "fresh Mach page was not writable and non-executable");
    require(f.engine.read64(address) == 0 &&
            static_cast<transmute::GuestMemory&>(f.engine).read8(address + 2 * page_size - 1) == 0,
            "Mach allocation was not zero filled and rounded to pages");
    f.engine.write64(address, 27);
    f.engine.write64(data + 8, address);
    require(f.call("_vm_allocate", {f.task, data + 8, page_size, 0}) == 3,
            "fixed overlapping allocation was accepted");
    require(f.call("_vm_protect", {f.task, address, page_size, 0, 7}) == 2,
            "writable executable mapping was accepted");
    require(f.call("_vm_protect", {f.task, address + 8, 1, 0, 5}) == 0,
            "page-rounded protection failed");
    const auto code = f.engine.query_region(address);
    const auto neighbor = f.engine.query_region(address + page_size);
    require(code && code->readable && !code->writable && code->executable &&
            neighbor && neighbor->writable && !neighbor->executable,
            "protection changed an adjacent page");
    require(f.call("_vm_protect", {f.task, address, page_size, 1, 1}) == 0 &&
            f.call("_vm_protect", {f.task, address, page_size, 0, 3}) == 2,
            "maximum protection did not prevent rights from being restored");
    require(f.call("_vm_protect", {f.task, address + page_size, 2 * page_size, 0, 1}) == 1 &&
            f.engine.query_region(address + page_size)->writable,
            "failed multi-page protection changed a valid prefix");
    require(f.call("_vm_deallocate", {f.task, address + page_size + 8, 1}) == 0 &&
            !f.engine.query_region(address + page_size) && f.engine.read64(address) == 27,
            "partial allocation deallocation damaged its neighbor");
    require(f.call("_vm_deallocate", {f.task, address, page_size}) == 0 &&
            !f.engine.query_region(address), "Mach page remained mapped after deallocation");
    require(f.call("_vm_deallocate", {f.task, UINT64_MAX - 4, 16}) == 1,
            "overflowing deallocation was accepted");
    require(f.call("_vm_protect", {f.task, data, page_size, 0, 1}) == 46,
            "Mach provider changed a mapping owned by another service");
    require(f.call("_vm_allocate", {f.task, data, 0, 1}) == 0 && f.engine.read64(data) == 0,
            "zero-size allocation did not return a null address");
}

void remap_capability_and_guards() {
    Fixture f;
    const auto source = f.allocate(page_size);
    const auto destination = f.allocate(2 * page_size);
    f.engine.write64(data, destination + page_size);
    f.engine.write32(data + 8, 0x11223344);
    f.engine.write32(data + 12, 0x55667788);
    f.remap_outputs(data + 8, data + 12);
    require(f.call("_vm_remap", {0, data, page_size, 0, 0x4000, f.task, source, 0}) == 16,
            "remap accepted an invalid target task");
    f.remap_outputs(0, data + 12);
    require(f.call("_vm_remap", {f.task, data, page_size, 0, 0x4000, f.task, source, 0}) == 1,
            "remap accepted an invalid protection output");
    f.remap_outputs(data + 8, data + 12);
    try {
        static_cast<void>(f.call("_vm_remap", {f.task, data, page_size, 0, 0x4000, f.task, source, 0}));
        throw std::runtime_error("shared remap succeeded without an engine alias capability");
    } catch (const std::runtime_error& error) {
        require(std::string_view(error.what()).find("shared guest-memory alias") != std::string_view::npos,
                "remap failed without a specific engine capability diagnostic");
    }
    require(f.engine.read64(data) == destination + page_size &&
            f.engine.read32(data + 8) == 0x11223344 && f.engine.read32(data + 12) == 0x55667788 &&
            f.engine.query_region(destination + page_size)->writable,
            "blocked remap modified output values or destination mappings");
}

// copy = TRUE: the target gets the source's bytes as of the remap (what a
// copy-on-write mapping shows) and the source's protection; later stores on
// either side stay on that side.
void remap_copy_on_write() {
    Fixture f;
    const auto source = f.allocate(page_size);
    const auto destination = f.allocate(page_size);
    f.engine.write64(source + 16, 0xc0ffee);
    f.engine.write64(data, destination);
    f.remap_outputs(data + 8, data + 12);
    require(f.call("_vm_remap", {f.task, data, page_size, 0, 0x4000, f.task, source, 1}) == 0,
            "copy-on-write remap over Mach pages failed");
    require(f.engine.read64(data) == destination && f.engine.read64(destination + 16) == 0xc0ffee,
            "the copy does not hold the source's bytes");
    require((f.engine.read32(data + 8) & 3) == 3, "the copy did not keep the source's protection");
    f.engine.write64(source + 16, 1);
    f.engine.write64(destination + 24, 2);
    require(f.engine.read64(destination + 16) == 0xc0ffee && f.engine.read64(source + 24) == 0,
            "the copy and its source share pages");
}

void remap_immutable_source() {
    Fixture f;
    const auto source = f.allocate(page_size);
    const auto destination = f.allocate(2 * page_size);
    f.engine.write64(source, 0x1122334455667788ULL);
    f.engine.write64(source + page_size - 8, 0x99aabbccddeeff00ULL);
    require(f.call("_vm_protect", {f.task, source, page_size, 0, 1}) == 0, "source protection failed");
    f.engine.write64(data, destination + page_size);
    f.remap_outputs(data + 8, data + 12);
    require(f.call("_vm_remap", {f.task, data, page_size, 0, 0x4000, f.task, source, 0}) == 0,
            "remap of an immutable source failed");
    const auto alias = destination + page_size;
    const auto region = f.engine.query_region(alias);
    require(f.engine.read64(alias) == 0x1122334455667788ULL &&
            f.engine.read64(alias + page_size - 8) == 0x99aabbccddeeff00ULL,
            "immutable remap did not reproduce the source bytes");
    require(region && region->readable && !region->writable && !region->executable,
            "immutable remap did not take the source protection");
    require(f.engine.read32(data + 8) == 1 && f.engine.read32(data + 12) == 1 && f.engine.read64(data) == alias,
            "immutable remap reported wrong protections or moved the target");
    require(f.engine.query_region(destination)->writable, "remap changed the page before its destination");
    // Any valid inheritance is accepted (V8 passes VM_INHERIT_NONE); others are invalid.
    require(f.call("_vm_protect", {f.task, source, page_size, 0, 3}) == 0, "source made writable");
    f.engine.write64(source, 0x0102030405060708ULL);
    require(f.call("_vm_protect", {f.task, source, page_size, 0, 1}) == 0, "source made read-only again");
    f.remap_outputs(data + 8, data + 12, 2);
    require(f.call("_vm_remap", {f.task, data, page_size, 0, 0x4000, f.task, source, 0}) == 0 &&
                f.engine.read64(alias) == 0x0102030405060708ULL,
            "remap with VM_INHERIT_NONE failed");
    f.remap_outputs(data + 8, data + 12, 3);
    require(f.call("_vm_remap", {f.task, data, page_size, 0, 0x4000, f.task, source, 0}) == 4,
            "remap accepted an invalid inheritance");
}

void remap_over_foreign_memory() {
    // VM_FLAGS_OVERWRITE over memory that is neither Mach VM pages nor mmap'd
    // (here image-like engine memory, and no mmap space on this engine):
    // KERN_NOT_SUPPORTED, the destination unchanged.
    Fixture f;
    const auto source = f.allocate(page_size);
    require(f.call("_vm_protect", {f.task, source, page_size, 0, 5}) == 0, "source protection failed");
    auto foreign = 0x540000000ULL;
    while (!f.engine.is_range_available(foreign, page_size)) foreign += page_size;
    f.engine.map_memory(foreign, page_size);
    f.engine.write64(foreign, 77);
    f.engine.write64(data, foreign);
    f.remap_outputs(data + 8, data + 12, 2);
    require(f.call("_vm_remap", {f.task, data, page_size, 0, 0x4000, f.task, source, 0}) == 46,
            "remap over foreign memory was not refused");
    require(f.engine.read64(foreign) == 77 && f.engine.read64(data) == foreign, "refused remap changed the destination");
}

void concurrent_allocations() {
    Fixture f;
    auto left = f.engine.fork_thread(0, 0);
    auto right = f.engine.fork_thread(0, 0);
    const auto allocate = f.context.resolve(system_library, "_vm_allocate");
    require(allocate.has_value(), "missing allocation export");
    std::exception_ptr failure;
    std::mutex failure_lock;
    const auto run = [&](transmute::ReferenceEngine& engine, std::uint64_t output) {
        try {
            const std::array<std::uint64_t, 4> arguments{f.task, output, page_size, 1};
            require(engine.invoke_callback(allocate->address, arguments) == 0,
                    "worker Mach allocation failed");
        } catch (...) {
            std::lock_guard lock(failure_lock);
            failure = std::current_exception();
        }
    };
    std::thread first([&] { run(*left, data); });
    std::thread second([&] { run(*right, data + 8); });
    first.join(); second.join();
    if (failure) std::rethrow_exception(failure);
    require(f.engine.read64(data) != f.engine.read64(data + 8),
            "concurrent allocations returned overlapping pages");
    const auto string = f.context.allocate_guest(4);
    f.engine.write32(string, 0x0062696c); // "lib".
    require(f.call("__dyld_shared_cache_contains_path", {string}) == 0,
            "Alchemy claimed an Apple shared-cache image");
}

// mach_vm_region_recurse: the containing region (no submaps, depth 0), the
// info flavor chosen by the count (v2 19 words, v0 16, short 12).
void region_recurse() {
    Fixture f;
    const auto address = f.allocate(2 * page_size);
    const auto out = data + 0x100, size = out + 8, depth = out + 16, count = out + 20, info = out + 64;
    const auto query = [&](std::uint64_t at, std::uint32_t words) {
        f.engine.write64(out, at);
        f.engine.write32(depth, 7);
        f.engine.write32(count, words);
        return f.call("_mach_vm_region_recurse", {f.task, out, size, depth, info, count});
    };
    require(query(address + 8, 19) == 0 && f.engine.read64(out) == address && f.engine.read64(size) == page_size &&
            f.engine.read32(depth) == 0 && f.engine.read32(count) == 19, "region_recurse v2 lookup failed");
    require(f.engine.read32(info) == 3 && f.engine.read32(info + 4) == 7 && f.engine.read32(info + 8) == 1 &&
            f.engine.read32(info + 24) == 1 && f.engine.read32(info + 40) == 1 &&
            (f.engine.read32(info + 44) >> 24) == 2 && f.engine.read32(info + 48) == 0,
            "region_recurse submap info fields wrong");
    require(f.call("_vm_protect", {f.task, address + page_size, page_size, 0, 1}) == 0, "protection failed");
    require(query(address + page_size, 17) == 0 && f.engine.read32(count) == 17 && f.engine.read32(info) == 1,
            "region_recurse v1 count or protection wrong");
    require(query(address, 18) == 0 && f.engine.read32(count) == 17, "region_recurse did not round down to v1");
    require(query(address, 12) == 0 && f.engine.read32(count) == 12 && f.engine.read32(info + 24) == 1 &&
            (f.engine.read32(info + 28) >> 24) == 2, "region_recurse short info wrong");
    require(query(address, 11) == 4, "region_recurse accepted a short count");
    require(query(0x7f0000000000ULL, 19) == 1, "region_recurse found a region at an unmapped address");
    require(f.call("_vm_region_recurse_64", {0, out, size, depth, info, count}) == 16, "invalid task was accepted");
}

// mach_vm_purgable_control over vm_allocate(VM_FLAGS_PURGABLE) objects.
void purgeable_control() {
    Fixture f;
    require(f.call("_vm_allocate", {f.task, data, 2 * page_size, 3}) == 0, "purgeable allocation failed");
    const auto object = f.engine.read64(data);
    const auto state = data + 0x200;
    const auto control = [&](std::uint64_t at, std::uint32_t operation, std::uint32_t value) {
        f.engine.write32(state, value);
        return f.call("_mach_vm_purgable_control", {f.task, at, operation, state});
    };
    f.engine.write64(object + page_size, 0x55);
    require(control(object + page_size, 0, 1) == 0 && f.engine.read32(state) == 0, "set volatile failed");
    require(control(object, 1, 9) == 0 && f.engine.read32(state) == 1, "get state failed");
    require(f.engine.read64(object + page_size) == 0x55, "volatile memory lost its contents before a purge");
    require(control(0, 2, 0) == 0 && f.engine.read64(object + page_size) == 0, "purge all kept volatile contents");
    require(control(object, 0, 0) == 0 && f.engine.read32(state) == 2, "purged object did not report empty");
    require(control(object, 0, 1 | (2U << 8)) == 0 && f.engine.read32(state) == 0, "volatile group state refused");
    require(control(object, 0, 1U << 20) == 4 && control(object, 0, 3) == 4, "invalid purgeable state accepted");
    require(control(object, 3, 0) == 4 && control(object, 9, 0) == 4, "kernel-only or unknown control accepted");
    require(control(data, 1, 0) == 4, "non-purgeable memory was accepted");
    require(control(0x7f0000000000ULL, 1, 0) == 1, "unmapped address was accepted");
    require(f.call("_vm_protect", {f.task, object, page_size, 0, 1}) == 0 && control(object, 0, 0) == 2,
            "set state on read-only memory was accepted");
    require(f.call("_vm_deallocate", {f.task, object, 2 * page_size}) == 0 && control(object, 1, 0) == 1,
            "deallocated purgeable object survived");
    const auto plain = f.allocate(page_size);
    require(alchemy::register_darwin_purgeable(f.context, plain, page_size, true) && control(plain, 1, 5) == 0 &&
            f.engine.read32(state) == 0, "registered purgeable object not controllable");
    require(alchemy::register_darwin_purgeable(f.context, plain, page_size, false) && control(plain, 1, 0) == 4,
            "unregistered purgeable object still controllable");
}

} // namespace

int main() {
    try {
        allocations_and_protections();
        remap_capability_and_guards();
        remap_copy_on_write();
        remap_immutable_source();
        remap_over_foreign_memory();
        concurrent_allocations();
        region_recurse();
        purgeable_control();
        std::cout << "Alchemy Mach VM tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
