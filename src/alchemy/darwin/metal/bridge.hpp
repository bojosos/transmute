#pragma once

// Objective-C side of Metal on Vulkan (owner: A5): helpers shared by the
// provider's binding files (metal.cpp, metal_objects.cpp,
// metal_descriptors.cpp). Internal to src/alchemy/metal.
//
// Every Metal object the guest sees is an instance of a host class
// registered here (instance size 8: state lives in ObjcRuntime::host_state)
// that adopts the matching Metal protocol (add_native_protocol), so
// conformsToProtocol:@protocol(MTLDevice) and friends answer YES. Methods are
// native (CpuState/GuestMemory handlers) and follow the Apple ARM64 calling
// convention described in include/alchemy/call_regs.hpp: structs larger than
// 16 bytes that are not HFAs (MTLRegion, MTLSize, MTLOrigin, MTLViewport,
// MTLScissorRect) arrive by reference, MTLClearColor in d0-d3, MTLSize
// results go through x8.
//
// Ownership follows Cocoa: new*/copy return +1, other getters return
// autoreleased or internally held objects. Guest objects a Metal object
// keeps (labels, descriptors' textures, the device) are released in the
// class's -dealloc, never from a payload destructor.

#include "alchemy/core_foundation.hpp"
#include "alchemy/darwin/metal/commands.hpp"
#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/pipeline.hpp"
#include "alchemy/darwin/metal/resources.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/registration_audit.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::metal::bridge {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
using Handler = std::function<void(CpuState&, GuestMemory&)>;

inline constexpr std::string_view metal_library = "/System/Library/Frameworks/Metal.framework/Versions/A/Metal";
inline constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";

// The process-wide bridge, created by install_metal_gui.
struct Runtime {
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;

    // A class of the Metal framework: the existing headless shell or a new
    // native class published as _OBJC_CLASS_$_<name> when `publish`.
    Address define_class(std::string_view name, std::string_view superclass = "NSObject", bool publish = true);
    // Takes its caller's location for the registration audit.
    void method(Address cls, std::string_view selector, Handler handler, bool class_method = false,
                std::string types = "@@:", const std::source_location& where = std::source_location::current());
    // -dealloc for host classes: releases the guest references the state
    // holds, then runs the superclass's -dealloc.
    void install_dealloc(Address cls);
    // A new instance (+1) of `cls` whose host state is `state`.
    template <class T>
    Address create(Address cls, std::shared_ptr<T> state) {
        const auto object = objc.allocate(cls);
        static_cast<void>(objc.host_state<T>(object, [&] { return state; }));
        return object;
    }
    Address retain(Address object) { return object ? objc.send(object, "retain").x0 : 0; }
    void release(Address object) { if (object) static_cast<void>(objc.send(object, "release")); }
    Address autorelease(Address object) { return object ? objc.send(object, "autorelease").x0 : 0; }
    // An autoreleased NSString.
    Address string(std::string_view text) { return autorelease(cf.string(text)); }
    [[nodiscard]] std::string utf8(Address string) { return string ? cf.utf8(string) : std::string{}; }
    // An autoreleased NSError-like object (domain MTLLibraryErrorDomain,
    // MTLCommandBufferErrorDomain, ...) with localizedDescription.
    Address error(std::string_view domain, std::int64_t code, std::string_view message);
    // Stores error(...) at *out when out is non-null.
    void set_error(GuestMemory& memory, Address out, std::string_view domain, std::int64_t code, std::string_view message);
    // Guest calls.
    [[nodiscard]] Address symbol(std::string_view name) const;
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> arguments);
    // _Block_copy / block->invoke(block, args...) then _Block_release.
    Address copy_block(Address block);
    void invoke_block(Address block, std::initializer_list<std::uint64_t> arguments, bool release = true);
};

[[nodiscard]] Runtime& runtime();
void set_runtime(std::unique_ptr<Runtime> value);

// ---- arguments ----------------------------------------------------------------------
// Integer/pointer argument `index` of a message (x0 = self, x1 = _cmd, x2 the
// first argument); 8-byte stack slots after x7.
inline std::uint64_t argument(const CpuState& state, const GuestMemory& memory, unsigned index) {
    return index < 8 ? state.x[index] : memory.read64(state.sp + 8ULL * (index - 8));
}
inline float single(const CpuState& state, unsigned index) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(state.v[index][0]));
}
inline double real(const CpuState& state, unsigned index) { return std::bit_cast<double>(state.v[index][0]); }
inline void return_single(CpuState& state, float value) { state.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; }
inline void return_real(CpuState& state, double value) { state.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; }
inline Region read_region(const GuestMemory& memory, Address pointer) {   // MTLRegion {origin, size}
    Region region;
    region.x = static_cast<std::uint32_t>(memory.read64(pointer));
    region.y = static_cast<std::uint32_t>(memory.read64(pointer + 8));
    region.z = static_cast<std::uint32_t>(memory.read64(pointer + 16));
    region.width = static_cast<std::uint32_t>(memory.read64(pointer + 24));
    region.height = static_cast<std::uint32_t>(memory.read64(pointer + 32));
    region.depth = static_cast<std::uint32_t>(memory.read64(pointer + 40));
    return region;
}
inline std::array<std::uint32_t, 3> read_size(const GuestMemory& memory, Address pointer) {   // MTLSize / MTLOrigin
    return {static_cast<std::uint32_t>(memory.read64(pointer)), static_cast<std::uint32_t>(memory.read64(pointer + 8)),
            static_cast<std::uint32_t>(memory.read64(pointer + 16))};
}

// ---- host state ----------------------------------------------------------------------
// Base of every Metal object's state: guest references released in -dealloc.
struct Held : ObjectPayload {
    std::mutex mutex;
    Address label{};
    std::vector<Address> owned;   // +1 references
    // Takes the references to release (dealloc).
    std::vector<Address> take() {
        std::lock_guard lock(mutex);
        auto result = std::move(owned);
        owned.clear();
        if (label) result.push_back(label);
        label = 0;
        return result;
    }
    void keep(Address object) { if (object) { std::lock_guard lock(mutex); owned.push_back(object); } }
};

// The typed state of a Metal object, or an explicit failure when `object`
// is not one of this provider's `what` objects.
template <class T>
std::shared_ptr<T> state(Address object, std::string_view what) {
    if (!object) throw std::runtime_error("Alchemy Metal: nil " + std::string(what));
    auto value = runtime().objc.find_host_state<T>(object);
    if (!value) throw std::runtime_error("Alchemy Metal: object is not an " + std::string(what) + " of this provider");
    return value;
}
template <class T>
std::shared_ptr<T> optional_state(Address object) {
    return object ? runtime().objc.find_host_state<T>(object) : nullptr;
}

// Label accessors shared by every Metal object class.
void install_label(Address cls);

// ---- descriptors (metal_descriptors.cpp) ------------------------------------------------
// Reads of descriptor objects created from the generated property table.
// A descriptor whose properties were never touched answers the defaults.
void install_descriptors(Runtime& runtime);
[[nodiscard]] std::uint64_t descriptor_u64(Address object, std::string_view property);
// The raw values (Value::bits) of several properties with one state lookup.
void descriptor_read(Address object, std::span<const std::string_view> properties,
                     std::span<std::array<std::uint64_t, 4>> out);
[[nodiscard]] bool descriptor_bool(Address object, std::string_view property);
[[nodiscard]] float descriptor_f32(Address object, std::string_view property);
[[nodiscard]] double descriptor_f64(Address object, std::string_view property);
[[nodiscard]] Address descriptor_object(Address object, std::string_view property);   // not retained
[[nodiscard]] std::array<double, 4> descriptor_color(Address object, std::string_view property);
[[nodiscard]] std::array<std::uint8_t, 4> descriptor_swizzle(Address object, std::string_view property);
// A child descriptor (colorAttachments, depthAttachment, vertexDescriptor)
// when it exists, 0 when it was never created or was set to nil.
[[nodiscard]] Address descriptor_child(Address object, std::string_view property);
// Element `index` of a descriptor array when it exists, 0 otherwise.
[[nodiscard]] Address descriptor_element(Address array, std::uint32_t index);
// MTLFunctionConstantValues contents.
[[nodiscard]] ConstantValues constant_values(Address object);
// MTLArgumentDescriptor array -> (index, dataType, arrayLength) entries.
struct ArgumentEntry { std::uint32_t index{}, data_type{}, array_length{}; };
[[nodiscard]] std::vector<ArgumentEntry> argument_entries(Address array);

// ---- objects (metal_objects.cpp) ---------------------------------------------------------
void install_objects(Runtime& runtime);
// The guest id<MTLDevice> for the system device (created on first call,
// immortal), or 0 with the failure logged once when no device exists.
[[nodiscard]] Address system_device_object();
// The host event of a guest MTLEvent / MTLSharedEvent.
[[nodiscard]] std::shared_ptr<Event> event_of(Address object);

// ---- g3: IOSurface-backed textures (surface_textures.cpp) ---------------------------------
// -[MTLDevice newTextureWithDescriptor:iosurface:plane:], -[MTLTexture
// iosurface] / -iosurfacePlane; after install_texture.
void install_surface_textures(Runtime& runtime);
// Tests and diagnostics: how often a texture's IOSurface backing copied the
// pages into the image (uploads) and the image into the pages (downloads),
// and whether the pages are imported (VK_EXT_external_memory_host) or staged.
struct SurfaceTextureStats { std::uint64_t uploads{}, downloads{}; bool imported{}; };
[[nodiscard]] std::optional<SurfaceTextureStats> surface_texture_stats(std::uint64_t texture_object);

// ---- g3: cross-process MTLSharedEvent (shared_events.cpp) -------------------------------
// -[MTLSharedEvent newSharedEventHandle], -[MTLDevice newSharedEventWithHandle:],
// MTLSharedEventHandle; after the device and the event classes.
void install_shared_events(Runtime& runtime);

// ---- QuartzCore (quartzcore_link.cpp) -----------------------------------------------------
// Registers a new MTLDevice object with QuartzCore's presentation contract
// (src/alchemy/darwin/quartzcore/quartzcore_vulkan.hpp) when that provider is built.
void publish_to_quartzcore(Address device_object, const std::shared_ptr<Device>& device);

// ---- completion (metal.cpp) ----------------------------------------------------------------
// Guest-visible completion: scheduled/completed handlers and shared-event
// listeners run on the run loop's completion thread (gui::CompletionThread).
struct Completion {
    Address command_buffer{};                 // retained until the handlers ran
    std::shared_ptr<CommandBuffer> commands;
    std::vector<Address> scheduled, completed;   // _Block_copy'd blocks
    std::vector<Address> signaled_events;     // guest MTLSharedEvent objects to re-check
};
void command_buffer_committed(Completion completion);
// Shared-event listeners. A listener made with a dispatch queue runs its
// blocks on that queue (dispatch_async_f through a provider entry that
// install_listener_dispatch registers); one without a queue runs them on the
// completion thread.
void add_listener(Address event, Address listener, std::uint64_t value, Address block);
void check_listeners(Address event);
void install_listener_dispatch(Runtime& r);
// Invokes a _Block_copy'd guest block (ownership passes) with `arguments` on
// the completion thread, from any thread (buffer deallocators).
void run_block_later(Address block, std::vector<std::uint64_t> arguments);
// Waits on behalf of a guest thread, polling for process stops.
void wait_for(const std::function<bool(std::uint64_t timeout_ns)>& wait, std::string_view operation);

} // namespace alchemy::metal::bridge
