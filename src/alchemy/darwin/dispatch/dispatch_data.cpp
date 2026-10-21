#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/object_store.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// libdispatch data objects (dispatch_data_t), imported by Qt Gui's Metal
// backend and a renderer's Metal GPU backend to hand shader
// libraries to -[MTLDevice newLibraryWithData:error:].
//
// A data object is an Objective-C object of class OS_dispatch_data (as on
// Darwin, so objc_retain/objc_release and dispatch_retain/dispatch_release
// both manage it) whose bytes are one contiguous guest buffer owned by the
// object. dispatch_data_create always copies: the documented contract lets the
// system invoke the destructor once it no longer needs the caller's buffer,
// which for a copy is immediately (DISPATCH_DATA_DESTRUCTOR_FREE frees it,
// _MUNMAP unmaps it, a block is submitted to the given queue).
// Concatenation and subranges copy as well, so every object has exactly one
// region and dispatch_data_apply visits it once.
namespace alchemy::export_fill {
namespace {

struct DispatchData : ObjectPayload {
    std::uint64_t buffer{}, size{};
};

} // namespace

void install_dispatch_data(Provider& p, ObjcRuntime& objc) {
    auto& process = p.process();
    if (process.resolve(libsystem_library, "_dispatch_data_create")) return;
    auto data_class = objc.find_class("OS_dispatch_data");
    if (!data_class) data_class = objc.register_native_class("OS_dispatch_data", objc.find_class("NSObject"), 16);

    // DISPATCH_DATA_EMPTY is &_dispatch_data_empty, an immortal object.
    const auto empty = p.data(libsystem_library, "__dispatch_data_empty", 64);
    process.main_engine().write64(empty, data_class);
    process.objects().adopt(empty, 64, data_class, ObjectKind::instance, std::make_shared<DispatchData>(), true);

    // DISPATCH_DATA_DESTRUCTOR_FREE / _MUNMAP are the values of these
    // variables: private marker addresses compared by dispatch_data_create.
    const auto markers = process.allocate_guest(32, 16);
    process.main_engine().fill_bytes(markers, 0, 32);
    const auto free_marker = markers, munmap_marker = markers + 16;
    process.main_engine().write64(p.data(libsystem_library, "__dispatch_data_destructor_free", 8), free_marker);
    process.main_engine().write64(p.data(libsystem_library, "__dispatch_data_destructor_munmap", 8), munmap_marker);

    const auto payload_of = [&process](std::uint64_t object) -> std::shared_ptr<DispatchData> {
        const auto record = object ? process.objects().find(object) : nullptr;
        return record ? std::dynamic_pointer_cast<DispatchData>(record->payload) : nullptr;
    };
    const auto require_data = [payload_of](std::uint64_t object) {
        auto value = payload_of(object);
        if (!value) throw std::runtime_error("Alchemy dispatch: object is not a dispatch_data_t");
        return value;
    };
    // A new object owning a copy of `bytes` (the empty object for none).
    const auto make = [&process, data_class, empty](const std::vector<std::uint8_t>& bytes) -> std::uint64_t {
        if (bytes.empty()) return empty;
        auto value = std::make_shared<DispatchData>();
        value->size = bytes.size();
        value->buffer = process.allocate_guest(bytes.size(), 16);
        process.calling_engine().write_bytes(value->buffer, bytes);
        const auto buffer = value->buffer;
        return process.objects().create(16, data_class, ObjectKind::instance, std::move(value),
                                        [&process, buffer](std::uint64_t) { process.release_guest(buffer); });
    };
    const auto contents = [&process](const DispatchData& value) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(value.size));
        if (value.size) process.calling_engine().read_bytes(value.buffer, bytes);
        return bytes;
    };

    // dispatch_data_t dispatch_data_create(const void* buffer, size_t size,
    //     dispatch_queue_t queue, dispatch_block_t destructor)
    p.libsystem("_dispatch_data_create", [p, make, free_marker, munmap_marker](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto buffer = s.x[0], size = s.x[1], queue = s.x[2], destructor = s.x[3];
        if (size && !buffer) throw std::runtime_error("Alchemy dispatch_data_create: null buffer");
        if (size > (1ULL << 32)) throw std::runtime_error("Alchemy dispatch_data_create: buffer larger than 4 GiB");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (size) m.read_bytes(buffer, bytes);
        const auto object = make(bytes);
        if (destructor == free_marker) {
            static_cast<void>(p.call(libsystem_library, "_free", {buffer}));
        } else if (destructor == munmap_marker) {
            static_cast<void>(p.call(libsystem_library, "_munmap", {buffer, size}));
        } else if (destructor) {
            const auto target = queue ? queue : p.call(libsystem_library, "_dispatch_get_global_queue", {0, 0});
            static_cast<void>(p.call(libsystem_library, "_dispatch_async", {target, destructor}));
        }
        return object;
    }, 0);

    p.libsystem("_dispatch_data_get_size", [require_data](CpuState& s, GuestMemory&) -> std::uint64_t {
        return require_data(s.x[0])->size;
    });

    // dispatch_data_t dispatch_data_create_map(dispatch_data_t, const void** buffer, size_t* size):
    // every object is contiguous, so the map is the object itself (retained).
    p.libsystem("_dispatch_data_create_map", [&process, require_data](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto value = require_data(s.x[0]);
        if (s.x[1]) m.write64(s.x[1], value->buffer);
        if (s.x[2]) m.write64(s.x[2], value->size);
        return process.objects().retain(s.x[0]);
    }, 0);

    p.libsystem("_dispatch_data_create_concat", [require_data, make, contents](CpuState& s, GuestMemory&) -> std::uint64_t {
        auto bytes = contents(*require_data(s.x[0]));
        const auto tail = contents(*require_data(s.x[1]));
        bytes.insert(bytes.end(), tail.begin(), tail.end());
        return make(bytes);
    }, 0);

    p.libsystem("_dispatch_data_create_subrange", [require_data, make, contents](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto bytes = contents(*require_data(s.x[0]));
        const auto offset = std::min<std::uint64_t>(s.x[1], bytes.size());
        const auto length = std::min<std::uint64_t>(s.x[2], bytes.size() - offset);
        return make(std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                              bytes.begin() + static_cast<std::ptrdiff_t>(offset + length)));
    }, 0);

    // dispatch_data_t dispatch_data_copy_region(dispatch_data_t, size_t location, size_t* offset)
    p.libsystem("_dispatch_data_copy_region", [&process, require_data](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto value = require_data(s.x[0]);
        if (s.x[2]) m.write64(s.x[2], 0);
        if (s.x[1] >= value->size && value->size) throw std::runtime_error("Alchemy dispatch_data_copy_region: location out of range");
        return process.objects().retain(s.x[0]);
    }, 0);

    // bool dispatch_data_apply(dispatch_data_t, dispatch_data_applier_t):
    // the applier block (region, offset, buffer, size) runs once per region.
    p.libsystem("_dispatch_data_apply", [&process, require_data](CpuState& s, GuestMemory& m) -> std::uint64_t {
        const auto value = require_data(s.x[0]);
        if (!value->size) return 1;
        const auto block = s.x[1];
        if (!block) throw std::runtime_error("Alchemy dispatch_data_apply: null applier");
        const std::array<std::uint64_t, 5> arguments{block, s.x[0], 0, value->buffer, value->size};
        // The applier returns bool in w0.
        return (EngineAdapter(process.calling_engine()).call(m.read64(block + 16), arguments) & 0xff) != 0;
    }, 0);

    // dispatch_retain / dispatch_release accept data objects too (a reference
    // handler of the dispatch provider, which owns both exports).
    if (process.resolve(libsystem_library, "_dispatch_retain"))
        add_dispatch_reference_handler(process, [&process, payload_of](std::uint64_t object, bool retain) {
            if (!payload_of(object)) return false;
            if (retain) static_cast<void>(process.objects().retain(object));
            else process.objects().release(object);
            return true;
        });
}

} // namespace alchemy::export_fill
