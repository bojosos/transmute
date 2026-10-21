#include "alchemy/darwin/metal/bridge.hpp"

#include "alchemy/engine_adapter.hpp"

#include "alchemy/darwin/metal/formats.hpp"
#include "alchemy/darwin/metal/interop.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/msl/compiler.hpp"
#include "alchemy/perf.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

// Metal object classes on the host layer (owner: A5): MTLDevice, MTLBuffer,
// MTLTexture, MTLSamplerState, MTLHeap, MTLLibrary, MTLFunction, pipeline
// and depth/stencil states, MTLCommandQueue, MTLCommandBuffer, the render,
// compute and blit encoders, MTLArgumentEncoder, MTLFence, MTLEvent,
// MTLSharedEvent(+Listener), MTLCaptureManager/Scope, MTLBinaryArchive and
// the NSError / reflection objects they return. See bridge.hpp for the
// calling-convention and ownership rules.
namespace alchemy::metal::bridge {
namespace {

// ---- states ----------------------------------------------------------------------------

struct DeviceState final : Held { std::shared_ptr<Device> device; Address name{}; };
struct BufferState final : Held { std::shared_ptr<Buffer> buffer; Address device{}; std::shared_ptr<Heap> heap; };
struct TextureState final : Held {
    std::shared_ptr<Texture> texture;
    Address device{}, parent{}, buffer{};
    std::uint32_t parent_level{}, parent_slice{};
    std::shared_ptr<Heap> heap;
};
struct SamplerObject final : Held { std::shared_ptr<Sampler> sampler; Address device{}; };
struct DepthStencilObject final : Held { std::shared_ptr<DepthStencilState> state; Address device{}; };
struct HeapState final : Held { std::shared_ptr<Heap> heap; Address device{}; };
struct LibraryState final : Held { std::shared_ptr<ShaderLibrary> library; Address device{}; };
struct FunctionState final : Held {
    std::shared_ptr<ShaderFunction> function;
    Address device{}, name{};
    bool specialized{};
};
struct RenderPipelineObject final : Held { std::shared_ptr<RenderPipeline> pipeline; Address device{}; };
struct ComputePipelineObject final : Held { std::shared_ptr<ComputePipeline> pipeline; Address device{}; };
struct QueueState final : Held { Address device{}; };
struct CommandBufferState final : Held {
    std::shared_ptr<CommandBuffer> commands;
    Address device{}, queue{};
    std::vector<Address> scheduled, completed, signaled_events;
    // presentDrawable:: the message (-present, -presentAtTime:,
    // -presentAfterMinimumDuration:) sent to the drawable once the command
    // buffer is submitted; `texture` is the drawable's texture when it is one
    // of this provider's (ALCHEMY_METAL_CAPTURE_DRAWABLES reads it back).
    // `linear_colorspace`: the drawable's layer colorspace is linear (asked
    // only while capturing).
    struct Present {
        Address drawable{};
        const char* selector{};
        double argument{};
        std::shared_ptr<Texture> texture;
        bool linear_colorspace{};
    };
    std::vector<Present> presents;
    bool committed{};
};
struct EncoderState final : Held {
    std::shared_ptr<CommandBuffer> commands;
    Address command_buffer{}, device{};
    EncoderKind kind{EncoderKind::none};
    std::uint64_t dispatch_type{};
    bool ended{};
};
struct FenceState final : Held { Address device{}; };
struct Listener { Address listener{}; std::uint64_t value{}; Address block{}; };
struct EventState final : Held {
    std::shared_ptr<Event> event;
    Address device{};
    std::vector<Listener> listeners;
};
struct ListenerState final : Held { Address queue{}; };
struct ArgumentEncoderState final : Held {
    Address device{}, buffer{};
    std::uint64_t offset{}, length{}, alignment{8};
    // An array of `count` elements answers indices index .. index + count - 1,
    // element e at offset + e * stride.
    struct Slot { std::uint64_t offset{}; std::uint32_t data_type{}; std::uint32_t count{1}; std::uint64_t stride{8}; };
    std::map<std::uint32_t, Slot> slots;
};
struct ScopeState final : Held { Address device{}, queue{}; };
struct CaptureState final : Held { Address default_scope{}; };
struct ArchiveState final : Held { Address device{}; };
struct LogStateState final : Held { Address device{}; std::int64_t level{}, buffer_size{}; };
struct SampleBufferState final : Held { Address device{}; std::shared_ptr<CounterSampleBuffer> samples; };
struct ErrorState final : Held { std::string domain, message; std::int64_t code{}; };
struct ArgumentState final : Held {
    std::string name;
    std::uint64_t type{}, access{}, index{}, data_size{}, alignment{}, data_type{}, texture_type{}, array_length{1};
    bool active{true}, depth{};
};
struct ReflectionState final : Held { Address vertex{}, fragment{}, kernel{}; };

// Class addresses.
struct Classes {
    Address device{}, buffer{}, texture{}, sampler{}, depth_stencil{}, heap{}, library{}, function{}, render_pipeline{},
        compute_pipeline{}, queue{}, command_buffer{}, render_encoder{}, compute_encoder{}, blit_encoder{},
        argument_encoder{}, fence{}, event{}, shared_event{}, listener{}, capture_manager{}, capture_scope{}, archive{},
        error{}, argument{}, render_reflection{}, compute_reflection{}, log_state{}, counter_set{}, counter{}, sample_buffer{};
} classes;
Address timestamp_set_object{};   // the one MTLCounterSet (immortal), created on first use
Address timestamp_set();

Address capture_manager_object{};
std::mutex device_mutex;
Address device_object{};
bool device_failure_reported{};

std::shared_ptr<Device> device_of_object(Address object) { return state<DeviceState>(object, "MTLDevice")->device; }

template <class T, class F>
Handler on(std::string_view what, F function) {
    return [what, function](CpuState& s, GuestMemory& m) { function(*state<T>(s.x[0], what), s, m); };
}

Address device_from(const Held& held);

// ---- small helpers -------------------------------------------------------------------------

std::vector<std::uint8_t> guest_bytes(GuestMemory& memory, Address pointer, std::uint64_t length) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (length) {
        if (!pointer) throw std::runtime_error("Alchemy Metal: null bytes pointer");
        memory.read_bytes(pointer, bytes);
    }
    return bytes;
}

std::shared_ptr<Buffer> buffer_of(Address object) {
    return object ? state<BufferState>(object, "MTLBuffer")->buffer : nullptr;
}
std::shared_ptr<Texture> texture_of_object(Address object) {
    return object ? state<TextureState>(object, "MTLTexture")->texture : nullptr;
}
std::shared_ptr<Sampler> sampler_of(Address object) {
    return object ? state<SamplerObject>(object, "MTLSamplerState")->sampler : nullptr;
}

Address keep_retained(Held& held, Address object) {
    if (object) held.keep(runtime().retain(object));
    return object;
}

// MTLStorageMode etc. as returned by resources.
void install_resource_queries(Address cls, bool texture) {
    auto& r = runtime();
    const auto options = [texture](Address object) -> ResourceOptions {
        if (texture) return state<TextureState>(object, "MTLTexture")->texture->descriptor.options;
        return state<BufferState>(object, "MTLBuffer")->buffer->options;
    };
    r.method(cls, "storageMode", [options](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(options(s.x[0]).storage);
    }, false, "Q@:");
    r.method(cls, "cpuCacheMode", [options](CpuState& s, GuestMemory&) { s.x[0] = options(s.x[0]).cpu_cache_mode; }, false, "Q@:");
    r.method(cls, "hazardTrackingMode", [options](CpuState& s, GuestMemory&) {
        // MTLHazardTrackingModeDefault resolves to Tracked for resources.
        const auto mode = options(s.x[0]).hazard_tracking;
        s.x[0] = mode ? mode : 2;
    }, false, "Q@:");
    r.method(cls, "resourceOptions", [options](CpuState& s, GuestMemory&) { s.x[0] = options(s.x[0]).encode(); }, false, "Q@:");
    // Residency and purgeability: every resource stays resident.
    r.method(cls, "setPurgeableState:", [](CpuState& s, GuestMemory&) { s.x[0] = 2; }, false, "Q@:Q");   // NonVolatile
    r.method(cls, "isAliasable", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    r.method(cls, "makeAliasable", [](CpuState&, GuestMemory&) {}, false, "v@:");
    r.method(cls, "heapOffset", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "Q@:");
}

// The device object a Metal object belongs to.
void install_device_getter(Address cls) {
    runtime().method(cls, "device", [](CpuState& s, GuestMemory&) {
        const auto held = runtime().objc.find_host_state<Held>(s.x[0]);
        s.x[0] = held ? device_from(*held) : 0;
    });
}

Address device_from(const Held& held) {
    if (const auto* value = dynamic_cast<const BufferState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const TextureState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const SamplerObject*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const DepthStencilObject*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const HeapState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const LibraryState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const FunctionState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const RenderPipelineObject*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const ComputePipelineObject*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const QueueState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const CommandBufferState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const EncoderState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const FenceState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const EventState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const ArgumentEncoderState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const ScopeState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const ArchiveState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const LogStateState*>(&held)) return value->device;
    if (const auto* value = dynamic_cast<const SampleBufferState*>(&held)) return value->device;
    return 0;
}

// ---- reflection objects (MTLArgument) --------------------------------------------------------

// MTLDataType of a reflected type (structs and pointers by kind).
std::uint64_t data_type(const msl::Reflection& reflection, std::uint32_t index) {
    if (index >= reflection.types.size()) return 0;
    const auto& type = reflection.types[index];
    using K = msl::Type::Kind;
    using S = msl::Scalar;
    if (type.kind == K::structure) return 1;
    if (type.kind == K::array) return 2;
    if (type.kind == K::pointer) return 60;
    std::uint64_t base = 0;
    switch (type.scalar) {
    case S::float32: base = 3; break;
    case S::half: base = 16; break;
    case S::int32: base = 29; break;
    case S::uint32: base = 33; break;
    case S::int16: base = 37; break;
    case S::uint16: base = 41; break;
    case S::int8: base = 45; break;
    case S::uint8: base = 49; break;
    case S::boolean: base = 53; break;
    case S::int64: base = 81; break;
    case S::uint64: base = 85; break;
    default: return 0;
    }
    if (type.kind == K::vector) return base + (type.components - 1);
    if (type.kind == K::matrix && (base == 3 || base == 16)) return base + 4 + (type.columns - 2) * 3 + (type.components - 2);
    return base;
}

std::uint64_t texture_type(msl::TextureType type) {
    using T = msl::TextureType;
    switch (type) {
    case T::texture1d: return 0;
    case T::texture1d_array: return 1;
    case T::texture2d: case T::depth2d: return 2;
    case T::texture2d_array: case T::depth2d_array: return 3;
    case T::texture2d_ms: case T::depth2d_ms: return 4;
    case T::texture_cube: case T::depth_cube: return 5;
    case T::texture_cube_array: case T::depth_cube_array: return 6;
    case T::texture3d: return 7;
    case T::texture2d_ms_array: case T::depth2d_ms_array: return 8;
    case T::texture_buffer: return 9;
    }
    return 2;
}

std::uint64_t access_of(msl::Access access) {
    switch (access) {
    case msl::Access::read: case msl::Access::sample: return 0;   // MTLArgumentAccessReadOnly / MTLBindingAccessReadOnly
    case msl::Access::read_write: return 1;
    case msl::Access::write: return 2;
    }
    return 0;
}

// NSArray<MTLArgument *> for a stage's reflection (+0, autoreleased).
Address arguments_array(const msl::Reflection* reflection) {
    auto& r = runtime();
    std::vector<Address> items;
    if (reflection) {
        const auto make = [&](std::shared_ptr<ArgumentState> value) {
            items.push_back(r.create(classes.argument, std::move(value)));
        };
        for (const auto& buffer : reflection->buffers) {
            auto value = std::make_shared<ArgumentState>();
            value->name = buffer.name;
            value->type = 0;   // MTLArgumentTypeBuffer
            value->access = access_of(buffer.access);
            value->index = buffer.index;
            value->active = buffer.used;
            if (buffer.type < reflection->types.size()) {
                value->data_size = reflection->types[buffer.type].size;
                value->alignment = std::max(1U, reflection->types[buffer.type].alignment);
            }
            value->data_type = data_type(*reflection, buffer.type);
            make(std::move(value));
        }
        for (const auto& texture : reflection->textures) {
            auto value = std::make_shared<ArgumentState>();
            value->name = texture.name;
            value->type = 2;   // MTLArgumentTypeTexture
            value->access = access_of(texture.access);
            value->index = texture.index;
            value->active = texture.used;
            value->texture_type = texture_type(texture.texture_type);
            value->array_length = texture.array_length;
            value->depth = texture.texture_type >= msl::TextureType::depth2d;
            // textureDataType: MTLDataTypeFloat/Half/Int/UInt/Short/UShort.
            switch (texture.sample_scalar) {
            case msl::Scalar::half: value->data_type = 16; break;
            case msl::Scalar::int32: value->data_type = 29; break;
            case msl::Scalar::uint32: value->data_type = 33; break;
            case msl::Scalar::int16: value->data_type = 37; break;
            case msl::Scalar::uint16: value->data_type = 41; break;
            default: value->data_type = 3; break;
            }
            make(std::move(value));
        }
        for (const auto& sampler : reflection->samplers) {
            auto value = std::make_shared<ArgumentState>();
            value->name = sampler.name;
            value->type = 3;   // MTLArgumentTypeSampler
            value->index = sampler.index;
            value->active = sampler.used;
            make(std::move(value));
        }
        for (const auto& memory : reflection->threadgroup_memory) {
            auto value = std::make_shared<ArgumentState>();
            value->name = memory.name;
            value->type = 1;   // MTLArgumentTypeThreadgroupMemory
            value->index = memory.index;
            make(std::move(value));
        }
    }
    const auto array = r.cf.array(items);
    for (const auto item : items) r.release(item);
    return r.autorelease(array);
}

// ---- argument encoders -----------------------------------------------------------------------

constexpr std::uint32_t data_type_pointer = 60, data_type_texture = 58, data_type_sampler = 59,
    data_type_render_pipeline = 78, data_type_compute_pipeline = 79, data_type_indirect_command_buffer = 80;

// Layout rule (shared with the MSL lowering of argument buffers): members in
// [[id(n)]] order; resources (pointers, textures, samplers) take 8 bytes at
// 8-byte alignment, data members their natural size and alignment, arrays
// arrayLength elements.
std::shared_ptr<ArgumentEncoderState> layout_arguments(const std::vector<ArgumentEntry>& entries) {
    auto state = std::make_shared<ArgumentEncoderState>();
    auto sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.index < b.index; });
    std::uint64_t offset = 0;
    for (const auto& entry : sorted) {
        std::uint64_t size = 8, alignment = 8;
        const bool resource = entry.data_type == data_type_pointer || entry.data_type == data_type_texture ||
                              entry.data_type == data_type_sampler || entry.data_type == data_type_render_pipeline ||
                              entry.data_type == data_type_compute_pipeline ||
                              entry.data_type == data_type_indirect_command_buffer;
        if (!resource) {
            size = data_type_size(entry.data_type);
            if (!size) throw std::runtime_error("Alchemy Metal: argument buffer member of MTLDataType " +
                                                std::to_string(entry.data_type) + " is not translated");
            alignment = size;
        }
        const auto count = std::max(1U, entry.array_length);
        offset = (offset + alignment - 1) / alignment * alignment;
        state->slots[entry.index] = {offset, entry.data_type, count, size};
        offset += size * count;
        state->alignment = std::max(state->alignment, alignment);
    }
    state->length = (offset + state->alignment - 1) / state->alignment * state->alignment;
    return state;
}

// ---- encoders --------------------------------------------------------------------------------

Stage stage_for(const EncoderState& encoder, bool fragment) {
    if (encoder.kind == EncoderKind::compute) return Stage::kernel;
    return fragment ? Stage::fragment : Stage::vertex;
}

void ensure_active(const EncoderState& encoder) {
    if (encoder.ended) throw std::runtime_error("Alchemy Metal: command encoder used after endEncoding");
}

// Binding methods shared by render (vertex/fragment) and compute encoders.
void install_bindings(Address cls, std::string_view prefix, bool fragment) {
    auto& r = runtime();
    const auto name = [&](std::string_view base) {
        // "setBuffer:offset:atIndex:" -> "setVertexBuffer:offset:atIndex:".
        if (prefix.empty()) return std::string(base);
        std::string result = "set";
        result.append(prefix);
        result.append(base.substr(3));
        return result;
    };
    const auto what = "MTLCommandEncoder";
    r.method(cls, name("setBuffer:offset:atIndex:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory&) {
        ensure_active(e);
        e.commands->set_buffer(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[4]), buffer_of(s.x[2]), s.x[3]);
    }), false, "v@:@QQ");
    r.method(cls, name("setBufferOffset:atIndex:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory&) {
        ensure_active(e);
        e.commands->set_buffer_offset(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[3]), s.x[2]);
    }), false, "v@:QQ");
    r.method(cls, name("setBuffers:offsets:withRange:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory& m) {
        ensure_active(e);
        for (std::uint64_t index = 0; index < s.x[5]; ++index)
            e.commands->set_buffer(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[4] + index),
                                   buffer_of(m.read64(s.x[2] + 8 * index)), s.x[3] ? m.read64(s.x[3] + 8 * index) : 0);
    }), false, "v@:^@^Q{_NSRange=QQ}");
    r.method(cls, name("setBytes:length:atIndex:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory& m) {
        ensure_active(e);
        const auto bytes = guest_bytes(m, s.x[2], s.x[3]);
        e.commands->set_bytes(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[4]), bytes);
    }), false, "v@:^vQQ");
    r.method(cls, name("setTexture:atIndex:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory&) {
        ensure_active(e);
        e.commands->set_texture(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[3]), texture_of_object(s.x[2]));
    }), false, "v@:@Q");
    r.method(cls, name("setTextures:withRange:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory& m) {
        ensure_active(e);
        for (std::uint64_t index = 0; index < s.x[4]; ++index)
            e.commands->set_texture(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[3] + index),
                                    texture_of_object(m.read64(s.x[2] + 8 * index)));
    }), false, "v@:^@{_NSRange=QQ}");
    r.method(cls, name("setSamplerState:atIndex:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory&) {
        ensure_active(e);
        e.commands->set_sampler(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[3]), sampler_of(s.x[2]));
    }), false, "v@:@Q");
    r.method(cls, name("setSamplerStates:withRange:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory& m) {
        ensure_active(e);
        for (std::uint64_t index = 0; index < s.x[4]; ++index)
            e.commands->set_sampler(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[3] + index),
                                    sampler_of(m.read64(s.x[2] + 8 * index)));
    }), false, "v@:^@{_NSRange=QQ}");
    // The clamps override the sampler state's lodMinClamp/lodMaxClamp for
    // this binding (floats in s0/s1, the index in x3).
    r.method(cls, name("setSamplerState:lodMinClamp:lodMaxClamp:atIndex:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory&) {
        ensure_active(e);
        e.commands->set_sampler(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[3]),
                                Sampler::with_lod_clamp(sampler_of(s.x[2]), single(s, 0), single(s, 1)));
    }), false, "v@:@ffQ");
    // float lodMinClamps[], float lodMaxClamps[] (x3, x4), NSRange in x5/x6.
    r.method(cls, name("setSamplerStates:lodMinClamps:lodMaxClamps:withRange:"), on<EncoderState>(what, [fragment](EncoderState& e, CpuState& s, GuestMemory& m) {
        ensure_active(e);
        for (std::uint64_t index = 0; index < s.x[6]; ++index) {
            const auto bits = [&](Address array) { const auto word = m.read32(array + 4 * index); float value; std::memcpy(&value, &word, 4); return value; };
            e.commands->set_sampler(stage_for(e, fragment), static_cast<std::uint32_t>(s.x[5] + index),
                                    Sampler::with_lod_clamp(sampler_of(m.read64(s.x[2] + 8 * index)), bits(s.x[3]), bits(s.x[4])));
        }
    }), false, "v@:^@^f^f{_NSRange=QQ}");
}

void install_encoder_common(Address cls) {
    auto& r = runtime();
    install_label(cls);
    install_device_getter(cls);
    const auto what = "MTLCommandEncoder";
    r.method(cls, "endEncoding", on<EncoderState>(what, [](EncoderState& e, CpuState&, GuestMemory&) {
        ensure_active(e);
        if (e.commands->encoder() == e.kind) e.commands->end_encoding();
        e.ended = true;
    }), false, "v@:");
    r.method(cls, "pushDebugGroup:", on<EncoderState>(what, [&r](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->push_debug_group(r.utf8(s.x[2]));
    }), false, "v@:@");
    r.method(cls, "popDebugGroup", on<EncoderState>(what, [](EncoderState& e, CpuState&, GuestMemory&) {
        e.commands->pop_debug_group();
    }), false, "v@:");
    r.method(cls, "insertDebugSignpost:", on<EncoderState>(what, [&r](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->insert_debug_signpost(r.utf8(s.x[2]));
    }), false, "v@:@");
    // Draw, dispatch and blit boundary sampling: -supportsCounterSampling:
    // answers NO for those points, so calling this is a client error.
    r.method(cls, "sampleCountersInBuffer:atSampleIndex:withBarrier:", [](CpuState&, GuestMemory&) {
        throw std::runtime_error("Alchemy Metal: unsupported: sampleCountersInBuffer:atSampleIndex:withBarrier: (only "
                                 "MTLCounterSamplingPointAtStageBoundary is supported)");
    }, false, "v@:@QB");
    // Residency: buffer device addresses need no residency in Vulkan, so
    // useResource:/useHeap: have nothing to do.
    for (const auto selector : {"useResource:usage:", "useResource:usage:stages:", "useResources:count:usage:",
                                "useResources:count:usage:stages:", "useHeap:", "useHeap:stages:", "useHeaps:count:",
                                "useHeaps:count:stages:"})
        r.method(cls, selector, [](CpuState&, GuestMemory&) {}, false, "v@:@Q");
    // Fences order work between encoders of one queue: every encoder already
    // begins with a full memory barrier on the single Vulkan queue.
    for (const auto selector : {"updateFence:", "waitForFence:", "updateFence:afterStages:", "waitForFence:beforeStages:"})
        r.method(cls, selector, [](CpuState& s, GuestMemory&) { static_cast<void>(state<FenceState>(s.x[2], "MTLFence")); },
                 false, "v@:@");
}

Address make_encoder(Address cls, CommandBufferState& buffer, Address buffer_object, EncoderKind kind) {
    auto& r = runtime();
    auto value = std::make_shared<EncoderState>();
    value->commands = buffer.commands;
    value->command_buffer = buffer_object;
    value->device = buffer.device;
    value->kind = kind;
    return r.autorelease(r.create(cls, std::move(value)));
}

// ---- counter samples (MTL*PassSampleBufferAttachmentDescriptor) -------------------------------------

constexpr std::uint64_t dont_sample = ~std::uint64_t{0};   // MTLCounterDontSample

std::shared_ptr<CounterSampleBuffer> sample_buffer_of(Address object) {
    if (!object) return nullptr;
    return state<SampleBufferState>(object, "MTLCounterSampleBuffer")->samples;
}

// The sample attachments of a pass descriptor's sampleBufferAttachments
// array: `start`/`end` name the index properties of one element.
EncoderSamples pass_samples(Address descriptor, std::initializer_list<std::string_view> start,
                            std::initializer_list<std::string_view> end) {
    EncoderSamples samples;
    const auto array = descriptor ? descriptor_child(descriptor, "sampleBufferAttachments") : 0;
    if (!array) return samples;
    for (std::uint32_t index = 0; index < 4; ++index) {
        const auto element = descriptor_element(array, index);
        if (!element) continue;
        const auto buffer = sample_buffer_of(descriptor_object(element, "sampleBuffer"));
        if (!buffer) continue;
        for (const auto name : start)
            if (const auto at = descriptor_u64(element, name); at != dont_sample) samples.start.push_back({buffer, static_cast<std::uint32_t>(at)});
        for (const auto name : end)
            if (const auto at = descriptor_u64(element, name); at != dont_sample) samples.end.push_back({buffer, static_cast<std::uint32_t>(at)});
    }
    return samples;
}

EncoderSamples render_samples(Address descriptor) {
    return pass_samples(descriptor, {"startOfVertexSampleIndex", "startOfFragmentSampleIndex"},
                        {"endOfVertexSampleIndex", "endOfFragmentSampleIndex"});
}

EncoderSamples encoder_samples(Address descriptor) {
    return pass_samples(descriptor, {"startOfEncoderSampleIndex"}, {"endOfEncoderSampleIndex"});
}

RenderPass render_pass(Address descriptor) {
    RenderPass pass;
    // Every property of an attachment in one descriptor read (one object
    // lookup instead of twelve).
    const auto attachment = [](Address object, Attachment& target, bool color, bool stencil) {
        static constexpr std::array<std::string_view, 11> common{"texture", "level", "slice", "depthPlane", "loadAction",
            "storeAction", "resolveTexture", "resolveLevel", "resolveSlice", "resolveDepthPlane", "clearColor"};
        std::array<std::string_view, 11> names = common;
        names[10] = color ? "clearColor" : stencil ? "clearStencil" : "clearDepth";
        std::array<std::array<std::uint64_t, 4>, 11> values{};
        descriptor_read(object, names, values);
        const auto u32 = [&](std::size_t index) { return static_cast<std::uint32_t>(values[index][0]); };
        target.texture = texture_of_object(values[0][0]);
        target.level = u32(1);
        target.slice = u32(2);
        target.depth_plane = u32(3);
        target.load_action = u32(4);
        target.store_action = u32(5);
        target.resolve_texture = texture_of_object(values[6][0]);
        target.resolve_level = u32(7);
        target.resolve_slice = u32(8);
        target.resolve_depth_plane = u32(9);
        if (color) {
            for (int c = 0; c < 4; ++c) target.clear_color[c] = std::bit_cast<double>(values[10][c]);
        } else if (stencil) target.clear_stencil = u32(10);
        else target.clear_depth = std::bit_cast<double>(values[10][0]);
    };
    if (const auto colors = descriptor_child(descriptor, "colorAttachments"))
        for (std::uint32_t index = 0; index < 8; ++index)
            if (const auto element = descriptor_element(colors, index)) attachment(element, pass.colors[index], true, false);
    if (const auto depth = descriptor_child(descriptor, "depthAttachment")) attachment(depth, pass.depth, false, false);
    if (const auto stencil = descriptor_child(descriptor, "stencilAttachment")) attachment(stencil, pass.stencil, false, true);
    static constexpr std::array<std::string_view, 5> pass_names{"renderTargetWidth", "renderTargetHeight",
        "renderTargetArrayLength", "defaultRasterSampleCount", "visibilityResultBuffer"};
    std::array<std::array<std::uint64_t, 4>, 5> values{};
    descriptor_read(descriptor, pass_names, values);
    pass.width = static_cast<std::uint32_t>(values[0][0]);
    pass.height = static_cast<std::uint32_t>(values[1][0]);
    pass.layers = static_cast<std::uint32_t>(values[2][0]);
    pass.default_sample_count = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(values[3][0]));
    pass.visibility = buffer_of(values[4][0]);
    pass.samples = render_samples(descriptor);
    return pass;
}

void install_render_encoder() {
    auto& r = runtime();
    const auto cls = classes.render_encoder;
    install_encoder_common(cls);
    install_bindings(cls, "Vertex", false);
    install_bindings(cls, "Fragment", true);
    const auto what = "MTLRenderCommandEncoder";
    const auto method = [&](std::string_view selector, auto function, std::string types = "v@:") {
        r.method(cls, selector, on<EncoderState>(what, [function](EncoderState& e, CpuState& s, GuestMemory& m) {
            ensure_active(e);
            function(e, s, m);
        }), false, std::move(types));
    };
    method("setRenderPipelineState:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_render_pipeline(state<RenderPipelineObject>(s.x[2], "MTLRenderPipelineState")->pipeline);
    }, "v@:@");
    const auto read_viewport = [](GuestMemory& m, Address pointer) {
        Viewport value;
        double fields[6];
        for (int index = 0; index < 6; ++index) fields[index] = std::bit_cast<double>(m.read64(pointer + 8 * index));
        value = {fields[0], fields[1], fields[2], fields[3], fields[4], fields[5]};
        return value;
    };
    method("setViewport:", [read_viewport](EncoderState& e, CpuState& s, GuestMemory& m) {
        const auto value = read_viewport(m, s.x[2]);
        e.commands->set_viewports({&value, 1});
    }, "v@:{MTLViewport=dddddd}");
    method("setViewports:count:", [read_viewport](EncoderState& e, CpuState& s, GuestMemory& m) {
        std::vector<Viewport> values;
        for (std::uint64_t index = 0; index < s.x[3]; ++index) values.push_back(read_viewport(m, s.x[2] + 48 * index));
        e.commands->set_viewports(values);
    }, "v@:^{MTLViewport=dddddd}Q");
    const auto read_scissor = [](GuestMemory& m, Address pointer) {
        return Scissor{m.read64(pointer), m.read64(pointer + 8), m.read64(pointer + 16), m.read64(pointer + 24)};
    };
    method("setScissorRect:", [read_scissor](EncoderState& e, CpuState& s, GuestMemory& m) {
        const auto value = read_scissor(m, s.x[2]);
        e.commands->set_scissors({&value, 1});
    }, "v@:{MTLScissorRect=QQQQ}");
    method("setScissorRects:count:", [read_scissor](EncoderState& e, CpuState& s, GuestMemory& m) {
        std::vector<Scissor> values;
        for (std::uint64_t index = 0; index < s.x[3]; ++index) values.push_back(read_scissor(m, s.x[2] + 32 * index));
        e.commands->set_scissors(values);
    }, "v@:^{MTLScissorRect=QQQQ}Q");
    method("setFrontFacingWinding:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_front_facing(static_cast<std::uint32_t>(s.x[2]));
    }, "v@:Q");
    method("setCullMode:", [](EncoderState& e, CpuState& s, GuestMemory&) { e.commands->set_cull_mode(static_cast<std::uint32_t>(s.x[2])); }, "v@:Q");
    method("setTriangleFillMode:", [](EncoderState&, CpuState& s, GuestMemory&) {
        if (s.x[2] != 0) throw std::runtime_error("Alchemy Metal: MTLTriangleFillModeLines is not translated");
    }, "v@:Q");
    method("setDepthClipMode:", [](EncoderState&, CpuState& s, GuestMemory&) {
        if (s.x[2] != 0) throw std::runtime_error("Alchemy Metal: MTLDepthClipModeClamp is not translated");
    }, "v@:Q");
    method("setDepthBias:slopeScale:clamp:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_depth_bias(single(s, 0), single(s, 1), single(s, 2));
    }, "v@:fff");
    method("setBlendColorRed:green:blue:alpha:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_blend_color(single(s, 0), single(s, 1), single(s, 2), single(s, 3));
    }, "v@:ffff");
    method("setDepthStencilState:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_depth_stencil(s.x[2] ? state<DepthStencilObject>(s.x[2], "MTLDepthStencilState")->state : nullptr);
    }, "v@:@");
    method("setStencilReferenceValue:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_stencil_reference(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[2]));
    }, "v@:I");
    method("setStencilFrontReferenceValue:backReferenceValue:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_stencil_reference(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]));
    }, "v@:II");
    method("setVisibilityResultMode:offset:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_visibility_mode(static_cast<std::uint32_t>(s.x[2]), s.x[3]);
    }, "v@:QQ");
    // Store actions were fixed at encoder creation (MTLStoreActionUnknown
    // stores); changing to a resolve later cannot be honoured.
    for (const auto selector : {"setColorStoreAction:atIndex:", "setDepthStoreAction:", "setStencilStoreAction:"})
        method(selector, [](EncoderState&, CpuState& s, GuestMemory&) {
            if (s.x[2] == 2 || s.x[2] == 3)
                throw std::runtime_error("Alchemy Metal: deferred multisample-resolve store actions are not translated");
        }, "v@:Q");
    method("drawPrimitives:vertexStart:vertexCount:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->draw(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]), static_cast<std::uint32_t>(s.x[4]), 1, 0);
    }, "v@:QQQ");
    method("drawPrimitives:vertexStart:vertexCount:instanceCount:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->draw(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]), static_cast<std::uint32_t>(s.x[4]),
                         static_cast<std::uint32_t>(s.x[5]), 0);
    }, "v@:QQQQ");
    method("drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->draw(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]), static_cast<std::uint32_t>(s.x[4]),
                         static_cast<std::uint32_t>(s.x[5]), static_cast<std::uint32_t>(s.x[6]));
    }, "v@:QQQQQ");
    method("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->draw_indexed(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]), static_cast<std::uint32_t>(s.x[4]),
                                 buffer_of(s.x[5]), s.x[6], 1, 0, 0);
    }, "v@:QQQ@Q");
    method("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
        [](EncoderState& e, CpuState& s, GuestMemory&) {
            e.commands->draw_indexed(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]),
                                     static_cast<std::uint32_t>(s.x[4]), buffer_of(s.x[5]), s.x[6], static_cast<std::uint32_t>(s.x[7]), 0, 0);
        }, "v@:QQQ@QQ");
    method("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
        [](EncoderState& e, CpuState& s, GuestMemory& m) {
            e.commands->draw_indexed(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]),
                                     static_cast<std::uint32_t>(s.x[4]), buffer_of(s.x[5]), s.x[6], static_cast<std::uint32_t>(s.x[7]),
                                     static_cast<std::int32_t>(m.read64(s.sp)), static_cast<std::uint32_t>(m.read64(s.sp + 8)));
        }, "v@:QQQ@QQqQ");
    method("drawPrimitives:indirectBuffer:indirectBufferOffset:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->draw_indirect(static_cast<std::uint32_t>(s.x[2]), buffer_of(s.x[3]), s.x[4]);
    }, "v@:Q@Q");
    method("drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:",
        [](EncoderState& e, CpuState& s, GuestMemory&) {
            e.commands->draw_indexed_indirect(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]), buffer_of(s.x[4]),
                                              s.x[5], buffer_of(s.x[6]), s.x[7]);
        }, "v@:QQ@Q@Q");
    method("memoryBarrierWithScope:afterStages:beforeStages:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->memory_barrier(s.x[2]);
    }, "v@:QQQ");
    method("memoryBarrierWithResources:count:afterStages:beforeStages:", [](EncoderState& e, CpuState&, GuestMemory&) {
        e.commands->memory_barrier(0);
    }, "v@:^@QQQ");
    method("textureBarrier", [](EncoderState& e, CpuState&, GuestMemory&) { e.commands->memory_barrier(2); }, "v@:");
}

void install_compute_encoder() {
    auto& r = runtime();
    const auto cls = classes.compute_encoder;
    install_encoder_common(cls);
    install_bindings(cls, "", false);
    const auto what = "MTLComputeCommandEncoder";
    const auto method = [&](std::string_view selector, auto function, std::string types = "v@:") {
        r.method(cls, selector, on<EncoderState>(what, [function](EncoderState& e, CpuState& s, GuestMemory& m) {
            ensure_active(e);
            function(e, s, m);
        }), false, std::move(types));
    };
    r.method(cls, "dispatchType", on<EncoderState>(what, [](EncoderState& e, CpuState& s, GuestMemory&) { s.x[0] = e.dispatch_type; }), false, "Q@:");
    method("setComputePipelineState:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_compute_pipeline(state<ComputePipelineObject>(s.x[2], "MTLComputePipelineState")->pipeline);
    }, "v@:@");
    method("setThreadgroupMemoryLength:atIndex:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->set_threadgroup_memory(static_cast<std::uint32_t>(s.x[3]), static_cast<std::uint32_t>(s.x[2]));
    }, "v@:QQ");
    method("dispatchThreadgroups:threadsPerThreadgroup:", [](EncoderState& e, CpuState& s, GuestMemory& m) {
        e.commands->dispatch_threadgroups(read_size(m, s.x[2]), read_size(m, s.x[3]));
    }, "v@:{MTLSize=QQQ}{MTLSize=QQQ}");
    method("dispatchThreads:threadsPerThreadgroup:", [](EncoderState& e, CpuState& s, GuestMemory& m) {
        e.commands->dispatch_threads(read_size(m, s.x[2]), read_size(m, s.x[3]));
    }, "v@:{MTLSize=QQQ}{MTLSize=QQQ}");
    method("dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
        [](EncoderState& e, CpuState& s, GuestMemory& m) {
            e.commands->dispatch_indirect(buffer_of(s.x[2]), s.x[3], read_size(m, s.x[4]));
        }, "v@:@Q{MTLSize=QQQ}");
    method("memoryBarrierWithScope:", [](EncoderState& e, CpuState& s, GuestMemory&) { e.commands->memory_barrier(s.x[2]); }, "v@:Q");
    method("memoryBarrierWithResources:count:", [](EncoderState& e, CpuState&, GuestMemory&) { e.commands->memory_barrier(0); }, "v@:^@Q");
}

void install_blit_encoder() {
    auto& r = runtime();
    const auto cls = classes.blit_encoder;
    install_encoder_common(cls);
    const auto what = "MTLBlitCommandEncoder";
    const auto method = [&](std::string_view selector, auto function, std::string types = "v@:") {
        r.method(cls, selector, on<EncoderState>(what, [function](EncoderState& e, CpuState& s, GuestMemory& m) {
            ensure_active(e);
            function(e, s, m);
        }), false, std::move(types));
    };
    const auto region = [](GuestMemory& m, Address origin, Address size) {
        const auto o = read_size(m, origin);
        const auto z = read_size(m, size);
        return Region{o[0], o[1], o[2], z[0], z[1], z[2]};
    };
    method("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->copy_buffer(buffer_of(s.x[2]), s.x[3], buffer_of(s.x[4]), s.x[5], s.x[6]);
    }, "v@:@Q@QQ");
    const auto buffer_to_texture = [](EncoderState& e, CpuState& s, GuestMemory& m, std::uint64_t options) {
        // x2 buffer, x3 offset, x4 bytesPerRow, x5 bytesPerImage, x6 &size,
        // x7 texture; stack: slice, level, &origin.
        const auto size = read_size(m, s.x[6]);
        const auto origin = read_size(m, m.read64(s.sp + 16));
        e.commands->copy_buffer_to_texture(buffer_of(s.x[2]), s.x[3], s.x[4], s.x[5],
            {origin[0], origin[1], origin[2], size[0], size[1], size[2]}, texture_of_object(s.x[7]),
            static_cast<std::uint32_t>(m.read64(s.sp)), static_cast<std::uint32_t>(m.read64(s.sp + 8)), options);
    };
    method("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
        [buffer_to_texture](EncoderState& e, CpuState& s, GuestMemory& m) { buffer_to_texture(e, s, m, 0); },
        "v@:@QQQ{MTLSize=QQQ}@QQ{MTLOrigin=QQQ}");
    method("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:options:",
        [buffer_to_texture](EncoderState& e, CpuState& s, GuestMemory& m) { buffer_to_texture(e, s, m, m.read64(s.sp + 24)); },
        "v@:@QQQ{MTLSize=QQQ}@QQ{MTLOrigin=QQQ}Q");
    const auto texture_to_buffer = [region](EncoderState& e, CpuState& s, GuestMemory& m, std::uint64_t options) {
        // x2 texture, x3 slice, x4 level, x5 &origin, x6 &size, x7 buffer;
        // stack: offset, bytesPerRow, bytesPerImage.
        e.commands->copy_texture_to_buffer(texture_of_object(s.x[2]), static_cast<std::uint32_t>(s.x[3]),
            static_cast<std::uint32_t>(s.x[4]), region(m, s.x[5], s.x[6]), buffer_of(s.x[7]), m.read64(s.sp),
            m.read64(s.sp + 8), m.read64(s.sp + 16), options);
    };
    method("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:",
        [texture_to_buffer](EncoderState& e, CpuState& s, GuestMemory& m) { texture_to_buffer(e, s, m, 0); },
        "v@:@QQ{MTLOrigin=QQQ}{MTLSize=QQQ}@QQQ");
    method("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:options:",
        [texture_to_buffer](EncoderState& e, CpuState& s, GuestMemory& m) { texture_to_buffer(e, s, m, m.read64(s.sp + 24)); },
        "v@:@QQ{MTLOrigin=QQQ}{MTLSize=QQQ}@QQQQ");
    method("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:",
        [region](EncoderState& e, CpuState& s, GuestMemory& m) {
            const auto origin = read_size(m, m.read64(s.sp + 16));
            e.commands->copy_texture(texture_of_object(s.x[2]), static_cast<std::uint32_t>(s.x[3]), static_cast<std::uint32_t>(s.x[4]),
                region(m, s.x[5], s.x[6]), texture_of_object(s.x[7]), static_cast<std::uint32_t>(m.read64(s.sp)),
                static_cast<std::uint32_t>(m.read64(s.sp + 8)), origin[0], origin[1], origin[2]);
        }, "v@:@QQ{MTLOrigin=QQQ}{MTLSize=QQQ}@QQ{MTLOrigin=QQQ}");
    method("copyFromTexture:toTexture:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->copy_texture_all(texture_of_object(s.x[2]), texture_of_object(s.x[3]));
    }, "v@:@@");
    method("copyFromTexture:sourceSlice:sourceLevel:toTexture:destinationSlice:destinationLevel:sliceCount:levelCount:",
        [](EncoderState& e, CpuState& s, GuestMemory& m) {
            const auto source = texture_of_object(s.x[2]);
            const auto destination = texture_of_object(s.x[5]);
            const auto slices = m.read64(s.sp), levels = m.read64(s.sp + 8);
            for (std::uint64_t level = 0; level < levels; ++level) {
                const auto extent = source->level_extent(static_cast<std::uint32_t>(s.x[4] + level));
                for (std::uint64_t slice = 0; slice < slices; ++slice)
                    e.commands->copy_texture(source, static_cast<std::uint32_t>(s.x[3] + slice), static_cast<std::uint32_t>(s.x[4] + level),
                        {0, 0, 0, extent.width, extent.height, extent.depth}, destination,
                        static_cast<std::uint32_t>(s.x[6] + slice), static_cast<std::uint32_t>(s.x[7] + level), 0, 0, 0);
            }
        }, "v@:@QQ@QQQQ");
    method("fillBuffer:range:value:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->fill_buffer(buffer_of(s.x[2]), s.x[3], s.x[4], static_cast<std::uint8_t>(s.x[5]));
    }, "v@:@{_NSRange=QQ}C");
    method("generateMipmapsForTexture:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        e.commands->generate_mipmaps(texture_of_object(s.x[2]));
    }, "v@:@");
    // resolveCounters:inRange:destinationBuffer:destinationOffset: (NSRange in x3/x4).
    method("resolveCounters:inRange:destinationBuffer:destinationOffset:", [](EncoderState& e, CpuState& s, GuestMemory&) {
        const auto samples = sample_buffer_of(s.x[2]);
        if (s.x[3] > UINT32_MAX || s.x[4] > UINT32_MAX) throw std::runtime_error("Alchemy Metal: resolveCounters: range too large");
        e.commands->resolve_counters(samples, static_cast<std::uint32_t>(s.x[3]), static_cast<std::uint32_t>(s.x[4]), buffer_of(s.x[5]), s.x[6]);
    }, "v@:@{_NSRange=QQ}@Q");
    // Host-visible memory is coherent and every command buffer ends with a
    // GPU->host barrier: synchronization is already complete. The checks
    // keep wrong objects explicit.
    method("synchronizeResource:", [](EncoderState&, CpuState& s, GuestMemory&) {
        if (!optional_state<BufferState>(s.x[2]) && !optional_state<TextureState>(s.x[2]))
            throw std::runtime_error("Alchemy Metal: synchronizeResource: with an object that is not a resource");
    }, "v@:@");
    method("synchronizeTexture:slice:level:", [](EncoderState&, CpuState& s, GuestMemory&) {
        static_cast<void>(state<TextureState>(s.x[2], "MTLTexture"));
    }, "v@:@QQ");
    method("optimizeContentsForGPUAccess:", [](EncoderState&, CpuState&, GuestMemory&) {}, "v@:@");
    method("optimizeContentsForCPUAccess:", [](EncoderState&, CpuState&, GuestMemory&) {}, "v@:@");
}

// ---- command buffers and queues ----------------------------------------------------------------

// ALCHEMY_METAL_CAPTURE_DRAWABLES=<directory> (diagnostics, e.g. on a locked
// session where the window cannot be captured): each presented drawable's
// texture is copied out at the end of its command buffer and written as
// <directory>/drawable-NNNN.bmp (the first 16 presents) and
// drawable-last.bmp (every present), as the display shows it: BGRA8/RGBA8
// (and _sRGB: their stores encode) bytes as stored, RGBA16Float and
// RGB10A2/BGR10A2 clamped to [0, 1] and quantized. Values are display
// encoded unless the drawable's CAMetalLayer colorspace is a linear one
// (its name contains "Linear", the rule QuartzCore presents by), in which
// case they are sRGB-encoded first. Blender's RGBA16Float layer uses
// kCGColorSpaceExtendedSRGB, whose values are already encoded.
std::filesystem::path capture_directory() {
    static const std::filesystem::path directory = [] {
        std::string value;
#ifdef _MSC_VER
        char* buffer = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&buffer, &length, "ALCHEMY_METAL_CAPTURE_DRAWABLES") == 0 && buffer) {
            value = buffer;
            std::free(buffer);
        }
#else
        if (const char* buffer = std::getenv("ALCHEMY_METAL_CAPTURE_DRAWABLES")) value = buffer;
#endif
        std::filesystem::path result(value);
        if (!value.empty()) {
            std::error_code error;
            std::filesystem::create_directories(result, error);
        }
        return result;
    }();
    return directory;
}

void write_bmp(const std::filesystem::path& path, const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
               bool rgba) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    const auto put = [&](std::uint64_t value, int bytes) {
        for (int index = 0; index < bytes; ++index) out.put(static_cast<char>((value >> (8 * index)) & 0xff));
    };
    const std::uint64_t image = 4ULL * width * height;
    out.put('B'); out.put('M'); put(54 + image, 4); put(0, 4); put(54, 4);
    put(40, 4); put(width, 4); put(static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)), 4);   // top-down
    put(1, 2); put(32, 2); put(0, 4); put(image, 4); put(2835, 4); put(2835, 4); put(0, 4); put(0, 4);
    std::vector<std::uint8_t> row(4ULL * width);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(row.data(), pixels + 4ULL * width * y, row.size());
        if (rgba) for (std::size_t x = 0; x < row.size(); x += 4) std::swap(row[x], row[x + 2]);
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
}

// Drawable texel -> display byte. `encode` applies the sRGB transfer
// function (linear colorspace content); alpha is never encoded.
std::uint8_t display_byte(float value, bool encode, bool alpha) {
    if (std::isnan(value)) value = 0;
    value = std::clamp(value, 0.0F, 1.0F);
    if (encode && !alpha)
        value = value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(value * 255.0F));
}

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    const float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                      : exponent == 31 ? (mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity())
                      : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

// Converts `count` drawable texels to RGBA8 bytes (R, G, B, A order).
std::vector<std::uint8_t> drawable_rgba8(std::uint32_t format, const std::uint8_t* texels, std::size_t count, bool linear) {
    std::vector<std::uint8_t> out(4 * count);
    const bool srgb_store = format == pixel_format::bgra8_unorm_srgb || format == pixel_format::rgba8_unorm_srgb;
    for (std::size_t index = 0; index < count; ++index) {
        std::uint8_t* pixel = out.data() + 4 * index;
        switch (format) {
        case pixel_format::bgra8_unorm: case pixel_format::bgra8_unorm_srgb:
        case pixel_format::rgba8_unorm: case pixel_format::rgba8_unorm_srgb: {
            const std::uint8_t* source = texels + 4 * index;
            const bool bgra = format == pixel_format::bgra8_unorm || format == pixel_format::bgra8_unorm_srgb;
            const std::uint8_t rgba[4] = {source[bgra ? 2 : 0], source[1], source[bgra ? 0 : 2], source[3]};
            for (int channel = 0; channel < 4; ++channel)
                pixel[channel] = linear && !srgb_store ? display_byte(rgba[channel] / 255.0F, true, channel == 3) : rgba[channel];
            break;
        }
        case pixel_format::rgba16_float: {
            std::uint16_t source[4];
            std::memcpy(source, texels + 8 * index, sizeof(source));
            for (int channel = 0; channel < 4; ++channel)
                pixel[channel] = display_byte(half_to_float(source[channel]), linear, channel == 3);
            break;
        }
        case pixel_format::rgb10a2_unorm: case pixel_format::bgr10a2_unorm: {
            std::uint32_t word{};
            std::memcpy(&word, texels + 4 * index, sizeof(word));
            // RGB10A2: R in bits 0-9; BGR10A2: B in bits 0-9.
            const float low = (word & 1023) / 1023.0F, mid = ((word >> 10) & 1023) / 1023.0F,
                        high = ((word >> 20) & 1023) / 1023.0F, alpha = (word >> 30) / 3.0F;
            const bool bgr = format == pixel_format::bgr10a2_unorm;
            const float rgba[4] = {bgr ? high : low, mid, bgr ? low : high, alpha};
            for (int channel = 0; channel < 4; ++channel) pixel[channel] = display_byte(rgba[channel], linear, channel == 3);
            break;
        }
        default: break;
        }
    }
    return out;
}

// Whether a drawable's CAMetalLayer colorspace holds linear values (only
// asked while ALCHEMY_METAL_CAPTURE_DRAWABLES is set). Drawables without a
// layer or colorspace count as display encoded.
bool drawable_linear_colorspace(Runtime& r, Address drawable) {
    const auto layer = r.objc.send_if_responds(drawable, r.objc.selector("layer"));
    if (!layer || !layer->x0) return false;
    const auto colorspace = r.objc.send_if_responds(layer->x0, r.objc.selector("colorspace"));
    if (!colorspace || !colorspace->x0) return false;
    const auto copy_name = r.process.resolve("/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
                                             "_CGColorSpaceCopyName");
    if (!copy_name) return false;
    const std::array<std::uint64_t, 1> arguments{colorspace->x0};
    const auto name = EngineAdapter(r.process.calling_engine()).call(copy_name->address, arguments);
    if (!name) return false;
    const auto text = r.utf8(name);
    const std::array<std::uint64_t, 1> release{name};
    static_cast<void>(r.cf.invoke("CFRelease", release));
    return text.find("Linear") != std::string::npos;
}

void capture_drawables(CommandBufferState& b) {
    const auto directory = capture_directory();
    if (directory.empty()) return;
    static std::atomic<std::uint32_t> counter{0};
    for (const auto& present : b.presents) {
        const auto& texture = present.texture;
        if (!texture) continue;
        const auto format = texture->descriptor.pixel_format;
        std::uint64_t texel{};
        switch (format) {
        case pixel_format::bgra8_unorm: case pixel_format::bgra8_unorm_srgb: case pixel_format::rgba8_unorm:
        case pixel_format::rgba8_unorm_srgb: case pixel_format::rgb10a2_unorm: case pixel_format::bgr10a2_unorm:
            texel = 4;
            break;
        case pixel_format::rgba16_float: texel = 8; break;
        default:
            std::cerr << "Alchemy Metal: drawable capture skips MTLPixelFormat " << format << '\n';
            continue;
        }
        const auto width = texture->descriptor.width, height = texture->descriptor.height;
        auto readback = Buffer::create(texture->device, texel * width * height, {StorageMode::shared, 0});
        b.commands->begin_blit();
        b.commands->copy_texture_to_buffer(texture, 0, 0, {0, 0, 0, width, height, 1}, readback, 0, texel * width,
                                           texel * width * height);
        b.commands->end_encoding();
        const auto number = counter.fetch_add(1);
        const bool linear = present.linear_colorspace;
        b.commands->on_complete([directory, readback, width, height, format, linear, number] {
            const auto pixels = drawable_rgba8(format, readback->contents(), static_cast<std::size_t>(width) * height, linear);
            if (number < 16) {
                char name[32];
                std::snprintf(name, sizeof(name), "drawable-%04u.bmp", number);
                write_bmp(directory / name, pixels.data(), width, height, true);
            }
            write_bmp(directory / "drawable-last.bmp", pixels.data(), width, height, true);
        });
    }
}

Address make_command_buffer(QueueState& queue, Address queue_object, bool retained) {
    auto& r = runtime();
    auto value = std::make_shared<CommandBufferState>();
    value->device = queue.device;
    value->queue = queue_object;
    value->commands = CommandBuffer::create(device_of_object(queue.device), retained);
    return r.autorelease(r.create(classes.command_buffer, std::move(value)));
}

void install_command_buffer() {
    auto& r = runtime();
    const auto cls = classes.command_buffer;
    install_label(cls);
    install_device_getter(cls);
    const auto what = "MTLCommandBuffer";
    const auto method = [&](std::string_view selector, auto function, std::string types = "v@:") {
        r.method(cls, selector, on<CommandBufferState>(what, function), false, std::move(types));
    };
    method("commandQueue", [](CommandBufferState& b, CpuState& s, GuestMemory&) { s.x[0] = b.queue; }, "@@:");
    method("retainedReferences", [](CommandBufferState& b, CpuState& s, GuestMemory&) { s.x[0] = b.commands->retained_references; }, "B@:");
    method("errorOptions", [](CommandBufferState&, CpuState& s, GuestMemory&) { s.x[0] = 0; }, "Q@:");
    method("logs", [](CommandBufferState&, CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    method("status", [](CommandBufferState& b, CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(b.commands->status());
    }, "Q@:");
    method("error", [&r](CommandBufferState& b, CpuState& s, GuestMemory&) {
        // MTLCommandBufferErrorDomain, MTLCommandBufferErrorInternal (1).
        s.x[0] = b.commands->status() == CommandBufferStatus::error
                     ? r.error("MTLCommandBufferErrorDomain", 1, b.commands->error()) : 0;
    }, "@@:");
    // GPU timestamps are not measured.
    for (const auto selector : {"kernelStartTime", "kernelEndTime", "GPUStartTime", "GPUEndTime"})
        method(selector, [](CommandBufferState&, CpuState& s, GuestMemory&) { return_real(s, 0.0); }, "d@:");
    method("renderCommandEncoderWithDescriptor:", [](CommandBufferState& b, CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        RenderPass pass;
        {
            perf::Scope timed(perf::mtl_encoder_descriptor_ticks);
            pass = render_pass(s.x[2]);
        }
        {
            perf::Scope timed(perf::mtl_encoder_begin_ticks);
            b.commands->begin_render(pass);
        }
        perf::Scope timed(perf::mtl_encoder_object_ticks);
        s.x[0] = make_encoder(classes.render_encoder, b, self, EncoderKind::render);
    }, "@@:@");
    const auto compute = [](CommandBufferState& b, CpuState& s, std::uint64_t dispatch, EncoderSamples samples = {}) {
        const auto self = s.x[0];
        b.commands->begin_compute(dispatch == 1, std::move(samples));
        s.x[0] = make_encoder(classes.compute_encoder, b, self, EncoderKind::compute);
        state<EncoderState>(s.x[0], "MTLComputeCommandEncoder")->dispatch_type = dispatch;
    };
    method("computeCommandEncoder", [compute](CommandBufferState& b, CpuState& s, GuestMemory&) { compute(b, s, 0); }, "@@:");
    method("computeCommandEncoderWithDispatchType:", [compute](CommandBufferState& b, CpuState& s, GuestMemory&) {
        compute(b, s, s.x[2]);
    }, "@@:Q");
    method("computeCommandEncoderWithDescriptor:", [compute](CommandBufferState& b, CpuState& s, GuestMemory&) {
        compute(b, s, s.x[2] ? descriptor_u64(s.x[2], "dispatchType") : 0, encoder_samples(s.x[2]));
    }, "@@:@");
    const auto blit = [](CommandBufferState& b, CpuState& s, EncoderSamples samples) {
        const auto self = s.x[0];
        b.commands->begin_blit(std::move(samples));
        s.x[0] = make_encoder(classes.blit_encoder, b, self, EncoderKind::blit);
    };
    method("blitCommandEncoder", [blit](CommandBufferState& b, CpuState& s, GuestMemory&) { blit(b, s, {}); }, "@@:");
    method("blitCommandEncoderWithDescriptor:", [blit](CommandBufferState& b, CpuState& s, GuestMemory&) {
        blit(b, s, encoder_samples(s.x[2]));
    }, "@@:@");
    method("enqueue", [](CommandBufferState& b, CpuState&, GuestMemory&) { b.commands->enqueue(); });
    method("addScheduledHandler:", [&r](CommandBufferState& b, CpuState& s, GuestMemory&) {
        if (b.committed) throw std::runtime_error("Alchemy Metal: addScheduledHandler: after commit");
        b.scheduled.push_back(r.copy_block(s.x[2]));
    }, "v@:@?");
    method("addCompletedHandler:", [&r](CommandBufferState& b, CpuState& s, GuestMemory&) {
        if (b.committed) throw std::runtime_error("Alchemy Metal: addCompletedHandler: after commit");
        b.completed.push_back(r.copy_block(s.x[2]));
    }, "v@:@?");
    // Metal documents presentDrawable: as scheduling the drawable's -present
    // (-presentAtTime:, -presentAfterMinimumDuration:) for when the command
    // buffer is scheduled. The drawable - QuartzCore's CAMetalDrawable
    // (quartzcore_vulkan.hpp) or a guest object - gets the message right
    // after the submission, which orders its presentation work after this
    // command buffer's on the shared queue.
    const auto present = [&r](CommandBufferState& b, CpuState& s, const char* selector) {
        if (b.committed) throw std::runtime_error("Alchemy Metal: presentDrawable: after commit");
        if (!s.x[2]) throw std::runtime_error("Alchemy Metal: presentDrawable: with a nil drawable");
        if (!r.objc.responds(s.x[2], r.objc.selector(selector)))
            throw std::runtime_error(std::string("Alchemy Metal: presentDrawable: with an object that does not respond to ") +
                                     selector);
        std::shared_ptr<Texture> drawn;
        if (const auto drawable_texture = r.objc.send_if_responds(s.x[2], r.objc.selector("texture")))
            if (const auto texture = optional_state<TextureState>(drawable_texture->x0)) {
                drawn = texture->texture;
                b.commands->retain(drawn);
            }
        const bool linear = drawn && !capture_directory().empty() && drawable_linear_colorspace(r, s.x[2]);
        b.presents.push_back({s.x[2], selector, real(s, 0), std::move(drawn), linear});
        keep_retained(b, s.x[2]);
    };
    method("presentDrawable:", [present](CommandBufferState& b, CpuState& s, GuestMemory&) { present(b, s, "present"); }, "v@:@");
    method("presentDrawable:atTime:", [present](CommandBufferState& b, CpuState& s, GuestMemory&) {
        present(b, s, "presentAtTime:");
    }, "v@:@d");
    method("presentDrawable:afterMinimumDuration:", [present](CommandBufferState& b, CpuState& s, GuestMemory&) {
        present(b, s, "presentAfterMinimumDuration:");
    }, "v@:@d");
    method("encodeSignalEvent:value:", [](CommandBufferState& b, CpuState& s, GuestMemory&) {
        const auto event = state<EventState>(s.x[2], "MTLEvent");
        b.commands->encode_signal(event->event, s.x[3]);
        if (std::find(b.signaled_events.begin(), b.signaled_events.end(), s.x[2]) == b.signaled_events.end())
            b.signaled_events.push_back(keep_retained(b, s.x[2]));
    }, "v@:@Q");
    method("encodeWaitForEvent:value:", [](CommandBufferState& b, CpuState& s, GuestMemory&) {
        b.commands->encode_wait(state<EventState>(s.x[2], "MTLEvent")->event, s.x[3]);
        keep_retained(b, s.x[2]);
    }, "v@:@Q");
    method("pushDebugGroup:", [&r](CommandBufferState& b, CpuState& s, GuestMemory&) { b.commands->push_debug_group(r.utf8(s.x[2])); }, "v@:@");
    method("popDebugGroup", [](CommandBufferState& b, CpuState&, GuestMemory&) { b.commands->pop_debug_group(); });
    method("commit", [&r](CommandBufferState& b, CpuState& s, GuestMemory&) {
        if (b.committed) throw std::runtime_error("Alchemy Metal: command buffer committed twice");
        b.committed = true;
        const auto self = s.x[0];
        bool submitted = true;
        try {
            capture_drawables(b);
            b.commands->commit();
        } catch (const VulkanError& failure) {
            // Device loss and submission failures surface as
            // MTLCommandBufferStatusError, as on Metal.
            submitted = false;
            std::cerr << "Alchemy Metal: command buffer failed: " << failure.what() << '\n';
        }
        // Drawables are presented only after a successful submission (a
        // failed command buffer leaves them unpresented, as on Metal); the
        // drawable stays retained by the command buffer until it completes.
        const auto presents = std::move(b.presents);
        b.presents.clear();
        if (submitted) {
            for (const auto& present : presents) {
                CallRegs registers;
                if (std::strcmp(present.selector, "present") != 0) registers.set_d(0, present.argument);
                static_cast<void>(r.objc.send(present.drawable, present.selector, std::move(registers)));
            }
        }
        Completion completion;
        completion.command_buffer = self;
        completion.commands = b.commands;
        completion.scheduled = std::move(b.scheduled);
        completion.completed = std::move(b.completed);
        completion.signaled_events = b.signaled_events;
        command_buffer_committed(std::move(completion));
    });
    method("waitUntilScheduled", [](CommandBufferState& b, CpuState&, GuestMemory&) {
        if (!b.committed) throw std::runtime_error("Alchemy Metal: waitUntilScheduled before commit");
    });
    method("waitUntilCompleted", [](CommandBufferState& b, CpuState&, GuestMemory&) {
        if (!b.committed) throw std::runtime_error("Alchemy Metal: waitUntilCompleted before commit");
        const auto commands = b.commands;
        wait_for([commands](std::uint64_t timeout) { return commands->wait(timeout); }, "waitUntilCompleted");
    });
}

void install_queue() {
    auto& r = runtime();
    const auto cls = classes.queue;
    install_label(cls);
    install_device_getter(cls);
    const auto what = "MTLCommandQueue";
    r.method(cls, "commandBuffer", on<QueueState>(what, [](QueueState& q, CpuState& s, GuestMemory&) {
        s.x[0] = make_command_buffer(q, s.x[0], true);
    }));
    r.method(cls, "commandBufferWithUnretainedReferences", on<QueueState>(what, [](QueueState& q, CpuState& s, GuestMemory&) {
        s.x[0] = make_command_buffer(q, s.x[0], false);
    }));
    r.method(cls, "commandBufferWithDescriptor:", on<QueueState>(what, [](QueueState& q, CpuState& s, GuestMemory&) {
        const bool retained = s.x[2] ? descriptor_bool(s.x[2], "retainedReferences") : true;
        s.x[0] = make_command_buffer(q, s.x[0], retained);
    }), false, "@@:@");
    r.method(cls, "insertDebugCaptureBoundary", [](CpuState&, GuestMemory&) {}, false, "v@:");
}

// ---- resources -----------------------------------------------------------------------------

TextureDescriptor texture_descriptor(Address object) {
    TextureDescriptor d;
    d.type = static_cast<TextureType>(descriptor_u64(object, "textureType"));
    d.pixel_format = static_cast<std::uint32_t>(descriptor_u64(object, "pixelFormat"));
    d.width = static_cast<std::uint32_t>(descriptor_u64(object, "width"));
    d.height = static_cast<std::uint32_t>(descriptor_u64(object, "height"));
    d.depth = static_cast<std::uint32_t>(descriptor_u64(object, "depth"));
    d.mipmap_levels = static_cast<std::uint32_t>(descriptor_u64(object, "mipmapLevelCount"));
    d.sample_count = static_cast<std::uint32_t>(descriptor_u64(object, "sampleCount"));
    d.array_length = static_cast<std::uint32_t>(descriptor_u64(object, "arrayLength"));
    d.usage = descriptor_u64(object, "usage");
    d.options = ResourceOptions::decode(descriptor_u64(object, "resourceOptions"));
    d.swizzle = descriptor_swizzle(object, "swizzle");
    return d;
}

Address new_buffer(Address device, ResourceOptions options, std::uint64_t length, std::shared_ptr<Heap> heap = nullptr) {
    auto& r = runtime();
    auto value = std::make_shared<BufferState>();
    value->buffer = Buffer::create(device_of_object(device), length, options);
    value->device = device;
    value->heap = std::move(heap);
    return r.create(classes.buffer, std::move(value));
}

Address new_texture_object_for(Address device, std::shared_ptr<Texture> texture) {
    auto value = std::make_shared<TextureState>();
    value->texture = std::move(texture);
    value->device = device;
    return runtime().create(classes.texture, std::move(value));
}

Address new_texture(Address device, Address descriptor) {
    if (!descriptor) throw std::runtime_error("Alchemy Metal: newTextureWithDescriptor: with a nil descriptor");
    const auto d = texture_descriptor(descriptor);
    const auto metal_device = device_of_object(device);
    if (d.type == TextureType::texture_buffer) {
        // A texture buffer without a buffer gets its own (Metal allocates one).
        const auto* info = pixel_format_info(d.pixel_format);
        if (!info) throw std::runtime_error("Alchemy Metal: unsupported texture buffer format");
        auto buffer = Buffer::create(metal_device, static_cast<std::uint64_t>(d.width) * info->block_bytes, d.options);
        return new_texture_object_for(device, Texture::create_buffer_texture(buffer, d, 0, 0));
    }
    return new_texture_object_for(device, Texture::create(metal_device, d));
}

void install_buffer() {
    auto& r = runtime();
    const auto cls = classes.buffer;
    install_label(cls);
    install_device_getter(cls);
    install_resource_queries(cls, false);
    const auto what = "MTLBuffer";
    r.method(cls, "contents", on<BufferState>(what, [](BufferState& b, CpuState& s, GuestMemory&) {
        // alchemy-fx: guest VA == host VA, so the Vulkan mapping is the guest pointer.
        s.x[0] = reinterpret_cast<std::uint64_t>(b.buffer->contents());
    }), false, "^v@:");
    r.method(cls, "length", on<BufferState>(what, [](BufferState& b, CpuState& s, GuestMemory&) { s.x[0] = b.buffer->length; }), false, "Q@:");
    r.method(cls, "allocatedSize", on<BufferState>(what, [](BufferState& b, CpuState& s, GuestMemory&) { s.x[0] = b.buffer->memory.size; }), false, "Q@:");
    r.method(cls, "gpuAddress", on<BufferState>(what, [](BufferState& b, CpuState& s, GuestMemory&) { s.x[0] = b.buffer->address; }), false, "Q@:");
    // Shared and managed buffers use coherent memory: modified ranges are
    // visible to the GPU at the next submission without a flush.
    r.method(cls, "didModifyRange:", on<BufferState>(what, [](BufferState& b, CpuState& s, GuestMemory&) {
        if (s.x[2] > b.buffer->length || s.x[3] > b.buffer->length - s.x[2])
            throw std::runtime_error("Alchemy Metal: didModifyRange: outside the buffer");
    }), false, "v@:{_NSRange=QQ}");
    r.method(cls, "heap", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    r.method(cls, "remoteStorageBuffer", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    r.method(cls, "addDebugMarker:range:", [](CpuState&, GuestMemory&) {}, false, "v@:@{_NSRange=QQ}");
    r.method(cls, "removeAllDebugMarkers", [](CpuState&, GuestMemory&) {}, false, "v@:");
    r.method(cls, "newTextureWithDescriptor:offset:bytesPerRow:", on<BufferState>(what, [](BufferState& b, CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        auto texture = Texture::create_buffer_texture(b.buffer, texture_descriptor(s.x[2]), s.x[3], s.x[4]);
        auto value = std::make_shared<TextureState>();
        value->texture = std::move(texture);
        value->device = b.device;
        value->buffer = self;
        keep_retained(*value, self);
        s.x[0] = runtime().create(classes.texture, std::move(value));
    }), false, "@@:@QQ");
}

void install_texture() {
    auto& r = runtime();
    const auto cls = classes.texture;
    install_label(cls);
    install_device_getter(cls);
    install_resource_queries(cls, true);
    const auto what = "MTLTexture";
    const auto getter = [&](std::string_view selector, auto function, const char* types = "Q@:") {
        r.method(cls, selector, on<TextureState>(what, [function](TextureState& t, CpuState& s, GuestMemory&) {
            s.x[0] = function(*t.texture, t);
        }), false, types);
    };
    getter("width", [](Texture& t, TextureState&) -> std::uint64_t { return t.descriptor.width; });
    getter("height", [](Texture& t, TextureState&) -> std::uint64_t { return t.descriptor.height; });
    getter("depth", [](Texture& t, TextureState&) -> std::uint64_t { return t.descriptor.depth; });
    getter("mipmapLevelCount", [](Texture& t, TextureState&) -> std::uint64_t { return t.descriptor.mipmap_levels; });
    getter("arrayLength", [](Texture& t, TextureState&) -> std::uint64_t { return t.descriptor.array_length; });
    getter("sampleCount", [](Texture& t, TextureState&) -> std::uint64_t { return t.descriptor.sample_count; });
    getter("pixelFormat", [](Texture& t, TextureState&) -> std::uint64_t { return t.descriptor.pixel_format; });
    getter("textureType", [](Texture& t, TextureState&) -> std::uint64_t { return static_cast<std::uint64_t>(t.descriptor.type); });
    getter("usage", [](Texture& t, TextureState&) -> std::uint64_t { return t.descriptor.usage; });
    getter("allocatedSize", [](Texture& t, TextureState&) -> std::uint64_t { return t.allocated_size(); });
    getter("isFramebufferOnly", [](Texture& t, TextureState&) -> std::uint64_t {
        return !(t.descriptor.usage & (texture_usage::shader_read | texture_usage::shader_write));
    }, "B@:");
    getter("allowGPUOptimizedContents", [](Texture&, TextureState&) -> std::uint64_t { return 1; }, "B@:");
    getter("isShareable", [](Texture&, TextureState&) -> std::uint64_t { return 0; }, "B@:");
    getter("compressionType", [](Texture&, TextureState&) -> std::uint64_t { return 0; });
    getter("swizzle", [](Texture& t, TextureState&) -> std::uint64_t {
        const auto& v = t.descriptor.swizzle;
        return v[0] | (v[1] << 8) | (v[2] << 16) | (static_cast<std::uint64_t>(v[3]) << 24);
    }, "{MTLTextureSwizzleChannels=CCCC}@:");
    getter("parentTexture", [](Texture&, TextureState& state) -> std::uint64_t { return state.parent; }, "@@:");
    getter("parentRelativeLevel", [](Texture&, TextureState& state) -> std::uint64_t { return state.parent_level; });
    getter("parentRelativeSlice", [](Texture&, TextureState& state) -> std::uint64_t { return state.parent_slice; });
    getter("buffer", [](Texture&, TextureState& state) -> std::uint64_t { return state.buffer; }, "@@:");
    getter("bufferOffset", [](Texture& t, TextureState&) -> std::uint64_t { return t.buffer_offset; });
    getter("bufferBytesPerRow", [](Texture& t, TextureState&) -> std::uint64_t { return t.bytes_per_row; });
    getter("gpuResourceID", [](Texture& t, TextureState&) -> std::uint64_t { return reinterpret_cast<std::uint64_t>(t.view); });
    getter("rootResource", [](Texture&, TextureState& state) -> std::uint64_t { return state.parent ? state.parent : state.buffer; }, "@@:");

    r.method(cls, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", on<TextureState>(what, [](TextureState& t, CpuState& s, GuestMemory& m) {
        const auto region = read_region(m, s.x[2]);
        const auto size = t.texture->copy_size(s.x[5], 0, region);
        const auto bytes = guest_bytes(m, s.x[4], size);
        t.texture->replace_region(region, static_cast<std::uint32_t>(s.x[3]), 0, bytes, s.x[5], 0);
    }), false, "v@:{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}Q^vQ");
    r.method(cls, "replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:", on<TextureState>(what,
        [](TextureState& t, CpuState& s, GuestMemory& m) {
            const auto region = read_region(m, s.x[2]);
            const auto size = t.texture->copy_size(s.x[6], s.x[7], region);
            const auto bytes = guest_bytes(m, s.x[5], size);
            t.texture->replace_region(region, static_cast<std::uint32_t>(s.x[3]), static_cast<std::uint32_t>(s.x[4]), bytes, s.x[6], s.x[7]);
        }), false, "v@:{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}QQ^vQQ");
    const auto get_bytes = [](TextureState& t, GuestMemory& m, Address pointer, std::uint64_t row, std::uint64_t image,
                              const Region& region, std::uint32_t level, std::uint32_t slice) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(t.texture->copy_size(row, image, region)));
        t.texture->get_bytes(bytes, row, image, region, level, slice);
        m.write_bytes(pointer, bytes);
    };
    r.method(cls, "getBytes:bytesPerRow:fromRegion:mipmapLevel:", on<TextureState>(what, [get_bytes](TextureState& t, CpuState& s, GuestMemory& m) {
        get_bytes(t, m, s.x[2], s.x[3], 0, read_region(m, s.x[4]), static_cast<std::uint32_t>(s.x[5]), 0);
    }), false, "v@:^vQ{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}Q");
    r.method(cls, "getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:", on<TextureState>(what,
        [get_bytes](TextureState& t, CpuState& s, GuestMemory& m) {
            get_bytes(t, m, s.x[2], s.x[3], s.x[4], read_region(m, s.x[5]), static_cast<std::uint32_t>(s.x[6]),
                      static_cast<std::uint32_t>(s.x[7]));
        }), false, "v@:^vQQ{MTLRegion={MTLOrigin=QQQ}{MTLSize=QQQ}}QQ");
    const auto view = [](TextureState& t, Address self, std::uint64_t format, std::uint64_t type, std::uint64_t level,
                         std::uint64_t levels, std::uint64_t slice, std::uint64_t slices, std::array<std::uint8_t, 4> swizzle) {
        auto texture = Texture::create_view(t.texture, static_cast<std::uint32_t>(format), static_cast<TextureType>(type),
            static_cast<std::uint32_t>(level), static_cast<std::uint32_t>(levels), static_cast<std::uint32_t>(slice),
            static_cast<std::uint32_t>(slices), swizzle);
        auto value = std::make_shared<TextureState>();
        value->texture = std::move(texture);
        value->device = t.device;
        value->parent = self;
        value->parent_level = static_cast<std::uint32_t>(level);
        value->parent_slice = static_cast<std::uint32_t>(slice);
        keep_retained(*value, self);
        return runtime().create(classes.texture, std::move(value));
    };
    r.method(cls, "newTextureViewWithPixelFormat:", on<TextureState>(what, [view](TextureState& t, CpuState& s, GuestMemory&) {
        const auto& d = t.texture->descriptor;
        s.x[0] = view(t, s.x[0], s.x[2], static_cast<std::uint64_t>(d.type), 0, d.mipmap_levels, 0, t.texture->layer_count(), d.swizzle);
    }), false, "@@:Q");
    r.method(cls, "newTextureViewWithPixelFormat:textureType:levels:slices:", on<TextureState>(what, [view](TextureState& t, CpuState& s, GuestMemory&) {
        s.x[0] = view(t, s.x[0], s.x[2], s.x[3], s.x[4], s.x[5], s.x[6], s.x[7], t.texture->descriptor.swizzle);
    }), false, "@@:QQ{_NSRange=QQ}{_NSRange=QQ}");
    r.method(cls, "newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:", on<TextureState>(what,
        [view](TextureState& t, CpuState& s, GuestMemory& m) {
            const auto packed = m.read32(s.sp);
            s.x[0] = view(t, s.x[0], s.x[2], s.x[3], s.x[4], s.x[5], s.x[6], s.x[7],
                          {static_cast<std::uint8_t>(packed), static_cast<std::uint8_t>(packed >> 8),
                           static_cast<std::uint8_t>(packed >> 16), static_cast<std::uint8_t>(packed >> 24)});
        }), false, "@@:QQ{_NSRange=QQ}{_NSRange=QQ}{MTLTextureSwizzleChannels=CCCC}");
}

// ---- functions, libraries, pipelines --------------------------------------------------------------

msl::CompileOptions compile_options(Address options) {
    auto& r = runtime();
    msl::CompileOptions result;
    if (!options) return result;
    const auto version = descriptor_u64(options, "languageVersion");
    result.language_major = static_cast<std::uint32_t>(version >> 16);
    result.language_minor = static_cast<std::uint32_t>(version & 0xffff);
    result.fast_math = descriptor_bool(options, "fastMathEnabled") && descriptor_u64(options, "mathMode") != 0;
    result.preserve_invariance = descriptor_bool(options, "preserveInvariance");
    if (const auto macros = descriptor_object(options, "preprocessorMacros")) {
        for (const auto& [key, value] : r.cf.dictionary_entries(macros)) {
            std::string text;
            if (r.cf.kind(value) == ObjectKind::string) text = r.utf8(value);
            else if (const auto number = r.cf.number_value(value)) {
                const auto rounded = std::llround(*number);
                text = static_cast<double>(rounded) == *number ? std::to_string(rounded) : std::to_string(*number);
            }
            result.macros.emplace_back(r.utf8(key), text);
        }
    }
    return result;
}

Address new_function(LibraryState& library, Address device, std::string_view name, const ConstantValues* values,
                     std::string& error) {
    const auto found = library.library->find(name);
    if (!found) { error = "Function " + std::string(name) + " was not found in the library"; return 0; }
    auto function = std::make_shared<ShaderFunction>(library.library->device, found);
    if (values) {
        if (auto failure = function->specialize(*values)) { error = *failure; return 0; }
    }
    auto value = std::make_shared<FunctionState>();
    value->function = std::move(function);
    value->device = device;
    value->specialized = values != nullptr;
    return runtime().create(classes.function, std::move(value));
}

std::shared_ptr<ShaderFunction> pipeline_function(Address object, std::string_view role) {
    if (!object) return nullptr;
    const auto value = state<FunctionState>(object, "MTLFunction");
    if (!value->specialized)
        for (const auto& constant : value->function->reflection().function_constants)
            if (constant.required)
                throw std::runtime_error(std::string(role) + " " + value->function->name() + " uses function constant " +
                                         constant.name + " but was created without MTLFunctionConstantValues");
    return value->function;
}

void install_library_and_function() {
    auto& r = runtime();
    install_label(classes.library);
    install_device_getter(classes.library);
    const auto library_what = "MTLLibrary";
    r.method(classes.library, "functionNames", on<LibraryState>(library_what, [&r](LibraryState& l, CpuState& s, GuestMemory&) {
        std::vector<Address> names;
        for (const auto& function : l.library->functions) names.push_back(r.cf.string(function->name));
        const auto array = r.cf.array(names);
        for (const auto name : names) r.release(name);
        s.x[0] = r.autorelease(array);
    }));
    r.method(classes.library, "type", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "q@:");   // executable
    r.method(classes.library, "installName", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    r.method(classes.library, "newFunctionWithName:", on<LibraryState>(library_what, [&r](LibraryState& l, CpuState& s, GuestMemory&) {
        std::string error;
        s.x[0] = new_function(l, l.device, r.utf8(s.x[2]), nullptr, error);
    }), false, "@@:@");
    r.method(classes.library, "newFunctionWithName:constantValues:error:", on<LibraryState>(library_what,
        [&r](LibraryState& l, CpuState& s, GuestMemory& m) {
            std::string error;
            const auto values = constant_values(s.x[3]);
            const auto error_out = s.x[4];
            s.x[0] = new_function(l, l.device, r.utf8(s.x[2]), &values, error);
            if (!s.x[0]) r.set_error(m, error_out, "MTLLibraryErrorDomain", 5, error);   // MTLLibraryErrorFunctionNotFound
        }), false, "@@:@@^@");
    r.method(classes.library, "newFunctionWithName:constantValues:completionHandler:", on<LibraryState>(library_what,
        [&r](LibraryState& l, CpuState& s, GuestMemory&) {
            std::string error;
            const auto values = constant_values(s.x[3]);
            const auto block = s.x[4];
            const auto function = new_function(l, l.device, r.utf8(s.x[2]), &values, error);
            const auto failure = function ? 0 : r.error("MTLLibraryErrorDomain", 5, error);
            r.invoke_block(block, {function, failure}, false);
            r.release(function);
        }), false, "v@:@@@?");
    r.method(classes.library, "newFunctionWithDescriptor:error:", on<LibraryState>(library_what,
        [&r](LibraryState& l, CpuState& s, GuestMemory& m) {
            std::string error;
            const auto descriptor = s.x[2];
            const auto error_out = s.x[3];
            const auto constants = descriptor_object(descriptor, "constantValues");
            const auto values = constant_values(constants);
            s.x[0] = new_function(l, l.device, r.utf8(descriptor_object(descriptor, "name")), constants ? &values : nullptr, error);
            if (!s.x[0]) r.set_error(m, error_out, "MTLLibraryErrorDomain", 5, error);
        }), false, "@@:@^@");

    const auto cls = classes.function;
    install_label(cls);
    install_device_getter(cls);
    const auto what = "MTLFunction";
    r.method(cls, "name", on<FunctionState>(what, [&r](FunctionState& f, CpuState& s, GuestMemory&) {
        std::lock_guard lock(f.mutex);
        if (!f.name) f.name = r.cf.string(f.function->name());
        s.x[0] = f.name;
    }));
    r.method(cls, "dealloc", [&r, cls](CpuState& s, GuestMemory&) {
        if (const auto value = optional_state<FunctionState>(s.x[0])) { r.release(value->name); value->name = 0; }
        for (const auto object : runtime().objc.find_host_state<Held>(s.x[0]) ? runtime().objc.find_host_state<Held>(s.x[0])->take()
                                                                               : std::vector<Address>{})
            r.release(object);
        static_cast<void>(r.objc.send_super(s.x[0], cls, r.objc.selector("dealloc")));
    }, false, "v@:");
    r.method(cls, "functionType", on<FunctionState>(what, [](FunctionState& f, CpuState& s, GuestMemory&) {
        // MTLFunctionTypeVertex = 1, Fragment = 2, Kernel = 3.
        s.x[0] = f.function->stage() == msl::Stage::vertex ? 1 : f.function->stage() == msl::Stage::fragment ? 2 : 3;
    }), false, "Q@:");
    r.method(cls, "options", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "Q@:");
    r.method(cls, "patchType", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "Q@:");
    r.method(cls, "newArgumentEncoderWithBufferIndex:", on<FunctionState>(what, [&r](FunctionState& f, CpuState& s, GuestMemory&) {
        const auto& reflection = f.function->reflection();
        // An argument buffer of samplers (Blender's SStruct): no
        // argument-table slot; its samplers fill one sampler array binding
        // (msl::ArgumentBufferSamplers). One 8-byte token per sampler id.
        for (const auto& samplers : reflection.argument_buffers) {
            if (argument_buffer_index(samplers, &f.function->constants) != s.x[2]) continue;
            auto encoder = std::make_shared<ArgumentEncoderState>();
            for (const auto& member : samplers.members)
                for (std::uint32_t element = 0; element < std::max(1U, member.count); ++element)
                    encoder->slots[member.id + element] = {8ULL * (member.id + element), data_type_sampler, 1};
            encoder->length = 8ULL * std::max(1U, samplers.count);
            encoder->alignment = 8;
            encoder->device = f.device;
            s.x[0] = r.create(classes.argument_encoder, std::move(encoder));
            return;
        }
        const auto found = std::find_if(reflection.buffers.begin(), reflection.buffers.end(),
                                        [&](const auto& buffer) { return buffer.index == s.x[2]; });
        if (found == reflection.buffers.end() || !found->argument_buffer) {
            std::string known;
            for (const auto& samplers : reflection.argument_buffers)
                known += " " + samplers.name + "=buffer(" + std::to_string(samplers.index) +
                         (samplers.index_function_constant ? "+fc" + std::to_string(*samplers.index_function_constant) : std::string()) +
                         ")->" + std::to_string(argument_buffer_index(samplers, &f.function->constants));
            for (const auto& buffer : reflection.buffers)
                if (buffer.argument_buffer) known += " " + buffer.name + "=buffer(" + std::to_string(buffer.index) + ")";
            throw std::runtime_error("Alchemy Metal: buffer(" + std::to_string(s.x[2]) + ") of " + f.function->name() +
                                     " is not an argument buffer (argument buffers:" + (known.empty() ? " none" : known) + ")");
        }
        // Buffer pointers and constant data (msl::BufferBinding::arguments):
        // one slot per [[id]], an array member's element e at id + e, at the
        // offsets clang gave the struct.
        auto encoder = std::make_shared<ArgumentEncoderState>();
        for (const auto& argument : found->arguments) {
            const auto kind = argument.pointer ? data_type_pointer
                                               : static_cast<std::uint32_t>(data_type(reflection, argument.type));
            for (std::uint32_t element = 0; element < std::max(1U, argument.count); ++element)
                encoder->slots[argument.id + element] = {argument.offset + element * argument.stride, kind, 1, argument.stride};
        }
        encoder->length = found->argument_length;
        encoder->alignment = std::max(8U, found->argument_alignment);
        encoder->device = f.device;
        s.x[0] = r.create(classes.argument_encoder, std::move(encoder));
    }), false, "@@:Q");
}

std::shared_ptr<RenderPipeline> render_pipeline(Address device, Address descriptor) {
    if (!descriptor) throw std::runtime_error("nil render pipeline descriptor");
    RenderPipelineDescriptor d;
    d.vertex = pipeline_function(descriptor_object(descriptor, "vertexFunction"), "vertexFunction");
    d.fragment = pipeline_function(descriptor_object(descriptor, "fragmentFunction"), "fragmentFunction");
    if (const auto colors = descriptor_child(descriptor, "colorAttachments"))
        for (std::uint32_t index = 0; index < 8; ++index) {
            const auto element = descriptor_element(colors, index);
            if (!element) continue;
            auto& color = d.colors[index];
            color.pixel_format = static_cast<std::uint32_t>(descriptor_u64(element, "pixelFormat"));
            color.blending = descriptor_bool(element, "blendingEnabled");
            color.source_rgb = static_cast<std::uint32_t>(descriptor_u64(element, "sourceRGBBlendFactor"));
            color.destination_rgb = static_cast<std::uint32_t>(descriptor_u64(element, "destinationRGBBlendFactor"));
            color.rgb_operation = static_cast<std::uint32_t>(descriptor_u64(element, "rgbBlendOperation"));
            color.source_alpha = static_cast<std::uint32_t>(descriptor_u64(element, "sourceAlphaBlendFactor"));
            color.destination_alpha = static_cast<std::uint32_t>(descriptor_u64(element, "destinationAlphaBlendFactor"));
            color.alpha_operation = static_cast<std::uint32_t>(descriptor_u64(element, "alphaBlendOperation"));
            color.write_mask = static_cast<std::uint32_t>(descriptor_u64(element, "writeMask"));
        }
    d.depth_format = static_cast<std::uint32_t>(descriptor_u64(descriptor, "depthAttachmentPixelFormat"));
    d.stencil_format = static_cast<std::uint32_t>(descriptor_u64(descriptor, "stencilAttachmentPixelFormat"));
    d.sample_count = static_cast<std::uint32_t>(descriptor_u64(descriptor, "rasterSampleCount"));
    d.alpha_to_coverage = descriptor_bool(descriptor, "alphaToCoverageEnabled");
    d.alpha_to_one = descriptor_bool(descriptor, "alphaToOneEnabled");
    d.rasterization = descriptor_bool(descriptor, "rasterizationEnabled");
    d.topology_class = static_cast<std::uint32_t>(descriptor_u64(descriptor, "inputPrimitiveTopology"));
    if (descriptor_u64(descriptor, "maxVertexAmplificationCount") > 1)
        throw std::runtime_error("vertex amplification is not translated");
    if (const auto vertex = descriptor_child(descriptor, "vertexDescriptor")) {
        const auto attributes = descriptor_child(vertex, "attributes");
        const auto layouts = descriptor_child(vertex, "layouts");
        for (std::uint32_t index = 0; index < 31; ++index) {
            if (const auto element = attributes ? descriptor_element(attributes, index) : 0) {
                const auto format = static_cast<std::uint32_t>(descriptor_u64(element, "format"));
                if (format)
                    d.attributes[index] = VertexAttribute{format, static_cast<std::uint32_t>(descriptor_u64(element, "offset")),
                                                          static_cast<std::uint32_t>(descriptor_u64(element, "bufferIndex"))};
            }
            if (const auto element = layouts ? descriptor_element(layouts, index) : 0)
                d.layouts[index] = VertexLayout{static_cast<std::uint32_t>(descriptor_u64(element, "stride")),
                                                static_cast<std::uint32_t>(descriptor_u64(element, "stepFunction")),
                                                static_cast<std::uint32_t>(descriptor_u64(element, "stepRate"))};
        }
    }
    d.label = runtime().utf8(descriptor_object(descriptor, "label"));
    return RenderPipeline::create(device_of_object(device), d);
}

Address render_reflection(const RenderPipeline& pipeline) {
    auto& r = runtime();
    auto value = std::make_shared<ReflectionState>();
    value->vertex = r.retain(arguments_array(&pipeline.vertex_reflection()));
    value->fragment = r.retain(arguments_array(pipeline.fragment_reflection()));
    value->keep(value->vertex);
    value->keep(value->fragment);
    return r.autorelease(r.create(classes.render_reflection, std::move(value)));
}

Address compute_reflection(const ComputePipeline& pipeline) {
    auto& r = runtime();
    auto value = std::make_shared<ReflectionState>();
    value->kernel = r.retain(arguments_array(&pipeline.function->reflection()));
    value->keep(value->kernel);
    return r.autorelease(r.create(classes.compute_reflection, std::move(value)));
}

std::shared_ptr<ComputePipeline> compute_pipeline(Address device, Address function, std::uint32_t max_threads) {
    return ComputePipeline::create(device_of_object(device), pipeline_function(function, "computeFunction"), max_threads);
}

Address new_compute_object(Address device, std::shared_ptr<ComputePipeline> pipeline) {
    auto value = std::make_shared<ComputePipelineObject>();
    value->pipeline = std::move(pipeline);
    value->device = device;
    return runtime().create(classes.compute_pipeline, std::move(value));
}

void install_pipeline_states() {
    auto& r = runtime();
    install_label(classes.render_pipeline);
    install_device_getter(classes.render_pipeline);
    r.method(classes.render_pipeline, "supportIndirectCommandBuffers", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    r.method(classes.render_pipeline, "gpuResourceID", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[0]; }, false, "Q@:");
    for (const auto selector : {"maxTotalThreadsPerObjectThreadgroup", "maxTotalThreadsPerMeshThreadgroup", "objectThreadExecutionWidth"})
        r.method(classes.render_pipeline, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "Q@:");
    const auto cls = classes.compute_pipeline;
    install_label(cls);
    install_device_getter(cls);
    const auto what = "MTLComputePipelineState";
    r.method(cls, "maxTotalThreadsPerThreadgroup", on<ComputePipelineObject>(what, [](ComputePipelineObject& p, CpuState& s, GuestMemory&) {
        s.x[0] = p.pipeline->max_total_threads;
    }), false, "Q@:");
    r.method(cls, "threadExecutionWidth", on<ComputePipelineObject>(what, [](ComputePipelineObject& p, CpuState& s, GuestMemory&) {
        s.x[0] = p.pipeline->thread_execution_width;
    }), false, "Q@:");
    r.method(cls, "staticThreadgroupMemoryLength", on<ComputePipelineObject>(what, [](ComputePipelineObject& p, CpuState& s, GuestMemory&) {
        s.x[0] = p.pipeline->static_threadgroup_memory;
    }), false, "Q@:");
    r.method(cls, "imageblockMemoryLengthForDimensions:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "Q@:{MTLSize=QQQ}");
    r.method(cls, "supportIndirectCommandBuffers", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    r.method(cls, "gpuResourceID", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[0]; }, false, "Q@:");

    // Reflection objects.
    const auto reflection_getter = [&](Address reflection_class, std::string_view selector, Address ReflectionState::* field) {
        r.method(reflection_class, selector, on<ReflectionState>("MTLPipelineReflection", [field](ReflectionState& value, CpuState& s, GuestMemory&) {
            s.x[0] = value.*field;
        }));
    };
    reflection_getter(classes.render_reflection, "vertexArguments", &ReflectionState::vertex);
    reflection_getter(classes.render_reflection, "fragmentArguments", &ReflectionState::fragment);
    reflection_getter(classes.compute_reflection, "arguments", &ReflectionState::kernel);
    // macOS 13's MTLBinding view: the same objects (MTLBindingType buffer,
    // threadgroup memory, texture and sampler have MTLArgumentType's values).
    reflection_getter(classes.render_reflection, "vertexBindings", &ReflectionState::vertex);
    reflection_getter(classes.render_reflection, "fragmentBindings", &ReflectionState::fragment);
    reflection_getter(classes.compute_reflection, "bindings", &ReflectionState::kernel);
    for (const auto selector : {"tileBindings", "objectBindings", "meshBindings"})
        r.method(classes.render_reflection, selector, [&r](CpuState& s, GuestMemory&) {
            s.x[0] = r.autorelease(r.cf.array({}));   // no tile, object or mesh stages
        }, false, "@@:");
    const auto arg = classes.argument;
    const auto argument_getter = [&](std::string_view selector, auto function, const char* types = "Q@:") {
        r.method(arg, selector, on<ArgumentState>("MTLArgument", [function](ArgumentState& a, CpuState& s, GuestMemory&) {
            s.x[0] = function(a);
        }), false, types);
    };
    argument_getter("type", [](ArgumentState& a) { return a.type; });
    argument_getter("access", [](ArgumentState& a) { return a.access; });
    argument_getter("index", [](ArgumentState& a) { return a.index; });
    argument_getter("isActive", [](ArgumentState& a) -> std::uint64_t { return a.active; }, "B@:");
    argument_getter("isUsed", [](ArgumentState& a) -> std::uint64_t { return a.active; }, "B@:");
    argument_getter("bufferAlignment", [](ArgumentState& a) { return a.alignment; });
    argument_getter("bufferDataSize", [](ArgumentState& a) { return a.data_size; });
    argument_getter("bufferDataType", [](ArgumentState& a) { return a.data_type; });
    argument_getter("bufferStructType", [](ArgumentState&) -> std::uint64_t { return 0; }, "@@:");
    argument_getter("bufferPointerType", [](ArgumentState&) -> std::uint64_t { return 0; }, "@@:");
    argument_getter("textureType", [](ArgumentState& a) { return a.texture_type; });
    argument_getter("textureDataType", [](ArgumentState& a) -> std::uint64_t { return a.data_type ? a.data_type : 3; });
    argument_getter("isArgument", [](ArgumentState&) -> std::uint64_t { return 0; }, "B@:");   // MTLBinding.argument
    argument_getter("isDepthTexture", [](ArgumentState& a) -> std::uint64_t { return a.depth; }, "B@:");
    argument_getter("arrayLength", [](ArgumentState& a) { return a.array_length; });
    argument_getter("threadgroupMemoryAlignment", [](ArgumentState&) -> std::uint64_t { return 16; });
    argument_getter("threadgroupMemoryDataSize", [](ArgumentState&) -> std::uint64_t { return 0; });
    r.method(arg, "name", on<ArgumentState>("MTLArgument", [&r](ArgumentState& a, CpuState& s, GuestMemory&) {
        std::lock_guard lock(a.mutex);
        if (!a.label) a.label = r.cf.string(a.name);
        s.x[0] = a.label;
    }));
}

void install_argument_encoder() {
    auto& r = runtime();
    const auto cls = classes.argument_encoder;
    install_label(cls);
    install_device_getter(cls);
    const auto what = "MTLArgumentEncoder";
    r.method(cls, "encodedLength", on<ArgumentEncoderState>(what, [](ArgumentEncoderState& a, CpuState& s, GuestMemory&) { s.x[0] = a.length; }), false, "Q@:");
    r.method(cls, "alignment", on<ArgumentEncoderState>(what, [](ArgumentEncoderState& a, CpuState& s, GuestMemory&) { s.x[0] = a.alignment; }), false, "Q@:");
    r.method(cls, "setArgumentBuffer:offset:", on<ArgumentEncoderState>(what, [](ArgumentEncoderState& a, CpuState& s, GuestMemory&) {
        a.buffer = s.x[2];
        a.offset = s.x[3];
    }), false, "v@:@Q");
    r.method(cls, "setArgumentBuffer:startOffset:arrayElement:", on<ArgumentEncoderState>(what, [](ArgumentEncoderState& a, CpuState& s, GuestMemory&) {
        a.buffer = s.x[2];
        a.offset = s.x[3] + s.x[4] * a.length;
    }), false, "v@:@QQ");
    const auto slot = [](ArgumentEncoderState& a, std::uint64_t index) -> std::uint8_t* {
        // The slot at `index`, or the element of the array slot that covers it.
        auto found = a.slots.upper_bound(static_cast<std::uint32_t>(index));
        if (found == a.slots.begin() || index >= (--found)->first + std::max(1U, found->second.count))
            throw std::runtime_error("Alchemy Metal: argument encoder has no index " + std::to_string(index));
        const auto offset = found->second.offset + (index - found->first) * found->second.stride;
        const auto buffer = buffer_of(a.buffer);
        if (!buffer || !buffer->contents()) throw std::runtime_error("Alchemy Metal: argument buffer must be CPU-visible");
        if (a.offset + offset + std::max<std::uint64_t>(1, found->second.stride) > buffer->length)
            throw std::runtime_error("Alchemy Metal: argument buffer too small");
        return buffer->contents() + a.offset + offset;
    };
    r.method(cls, "setBuffer:offset:atIndex:", on<ArgumentEncoderState>(what, [slot](ArgumentEncoderState& a, CpuState& s, GuestMemory&) {
        const auto buffer = buffer_of(s.x[2]);
        const std::uint64_t address = buffer ? buffer->address + s.x[3] : 0;
        std::memcpy(slot(a, s.x[4]), &address, 8);
    }), false, "v@:@QQ");
    r.method(cls, "setBuffers:offsets:withRange:", on<ArgumentEncoderState>(what, [slot](ArgumentEncoderState& a, CpuState& s, GuestMemory& m) {
        for (std::uint64_t index = 0; index < s.x[5]; ++index) {
            const auto buffer = buffer_of(m.read64(s.x[2] + 8 * index));
            const std::uint64_t address = buffer ? buffer->address + m.read64(s.x[3] + 8 * index) : 0;
            std::memcpy(slot(a, s.x[4] + index), &address, 8);
        }
    }), false, "v@:^@^Q{_NSRange=QQ}");
    r.method(cls, "constantDataAtIndex:", on<ArgumentEncoderState>(what, [slot](ArgumentEncoderState& a, CpuState& s, GuestMemory&) {
        s.x[0] = reinterpret_cast<std::uint64_t>(slot(a, s.x[2]));
    }), false, "^v@:Q");
    // Samplers: an opaque token per slot (sampler_token), resolved when a
    // draw binds the buffer to an argument buffer of samplers.
    const auto sampler_slot = [slot](ArgumentEncoderState& a, std::uint64_t index, Address object) {
        const auto found = a.slots.find(static_cast<std::uint32_t>(index));
        if (found != a.slots.end() && found->second.data_type != data_type_sampler)
            throw std::runtime_error("Alchemy Metal: argument encoder index " + std::to_string(index) + " is not a sampler");
        const auto token = sampler_token(object ? sampler_of(object) : nullptr);
        std::memcpy(slot(a, index), &token, 8);
    };
    r.method(cls, "setSamplerState:atIndex:", on<ArgumentEncoderState>(what, [sampler_slot](ArgumentEncoderState& a, CpuState& s, GuestMemory&) {
        sampler_slot(a, s.x[3], s.x[2]);
    }), false, "v@:@Q");
    r.method(cls, "setSamplerStates:withRange:", on<ArgumentEncoderState>(what, [sampler_slot](ArgumentEncoderState& a, CpuState& s, GuestMemory& m) {
        for (std::uint64_t index = 0; index < s.x[4]; ++index) sampler_slot(a, s.x[3] + index, m.read64(s.x[2] + 8 * index));
    }), false, "v@:^@{_NSRange=QQ}");
    // Textures inside argument buffers need a bindless descriptor heap the
    // MSL contract does not define yet.
    for (const auto selector : {"setTexture:atIndex:", "setTextures:withRange:", "setRenderPipelineState:atIndex:",
                                "setComputePipelineState:atIndex:", "setIndirectCommandBuffer:atIndex:",
                                "setAccelerationStructure:atIndex:", "setVisibleFunctionTable:atIndex:",
                                "setIntersectionFunctionTable:atIndex:"})
        r.method(cls, selector, [selector](CpuState&, GuestMemory&) {
            throw std::runtime_error(std::string("Alchemy Metal: -[MTLArgumentEncoder ") + selector +
                                     "] (resources other than buffers in argument buffers) is not translated");
        }, false, "v@:@Q");
    r.method(cls, "newArgumentEncoderForBufferAtIndex:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "@@:Q");
}

// ---- small objects: samplers, depth/stencil, heaps, fences, events, capture, archives ---------------

DepthStencilState depth_stencil(Address descriptor) {
    DepthStencilState value;
    value.depth_compare = static_cast<std::uint32_t>(descriptor_u64(descriptor, "depthCompareFunction"));
    value.depth_write = descriptor_bool(descriptor, "depthWriteEnabled");
    const auto stencil = [](Address object) {
        StencilState result;
        if (!object) return result;
        result.compare = static_cast<std::uint32_t>(descriptor_u64(object, "stencilCompareFunction"));
        result.stencil_failure = static_cast<std::uint32_t>(descriptor_u64(object, "stencilFailureOperation"));
        result.depth_failure = static_cast<std::uint32_t>(descriptor_u64(object, "depthFailureOperation"));
        result.pass = static_cast<std::uint32_t>(descriptor_u64(object, "depthStencilPassOperation"));
        result.read_mask = static_cast<std::uint32_t>(descriptor_u64(object, "readMask"));
        result.write_mask = static_cast<std::uint32_t>(descriptor_u64(object, "writeMask"));
        return result;
    };
    value.front = stencil(descriptor_child(descriptor, "frontFaceStencil"));
    value.back = stencil(descriptor_child(descriptor, "backFaceStencil"));
    value.label = runtime().utf8(descriptor_object(descriptor, "label"));
    return value;
}

SamplerDescriptor sampler_descriptor(Address descriptor) {
    SamplerDescriptor d;
    d.min_filter = static_cast<std::uint32_t>(descriptor_u64(descriptor, "minFilter"));
    d.mag_filter = static_cast<std::uint32_t>(descriptor_u64(descriptor, "magFilter"));
    d.mip_filter = static_cast<std::uint32_t>(descriptor_u64(descriptor, "mipFilter"));
    d.max_anisotropy = static_cast<std::uint32_t>(descriptor_u64(descriptor, "maxAnisotropy"));
    d.address = {static_cast<std::uint32_t>(descriptor_u64(descriptor, "sAddressMode")),
                 static_cast<std::uint32_t>(descriptor_u64(descriptor, "tAddressMode")),
                 static_cast<std::uint32_t>(descriptor_u64(descriptor, "rAddressMode"))};
    d.border_color = static_cast<std::uint32_t>(descriptor_u64(descriptor, "borderColor"));
    d.normalized_coordinates = descriptor_bool(descriptor, "normalizedCoordinates");
    d.lod_min = descriptor_f32(descriptor, "lodMinClamp");
    d.lod_max = descriptor_f32(descriptor, "lodMaxClamp");
    d.lod_average = descriptor_bool(descriptor, "lodAverage");
    d.compare_function = static_cast<std::uint32_t>(descriptor_u64(descriptor, "compareFunction"));
    d.support_argument_buffers = descriptor_bool(descriptor, "supportArgumentBuffers");
    d.label = runtime().utf8(descriptor_object(descriptor, "label"));
    return d;
}

void install_small_objects() {
    auto& r = runtime();
    for (const auto cls : {classes.sampler, classes.depth_stencil, classes.fence, classes.event, classes.shared_event,
                           classes.archive, classes.heap}) {
        install_label(cls);
        install_device_getter(cls);
    }
    r.method(classes.sampler, "gpuResourceID", on<SamplerObject>("MTLSamplerState", [](SamplerObject& value, CpuState& s, GuestMemory&) {
        s.x[0] = reinterpret_cast<std::uint64_t>(value.sampler->sampler);
    }), false, "Q@:");

    // Heaps.
    const auto heap_what = "MTLHeap";
    r.method(classes.heap, "size", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) { s.x[0] = h.heap->size; }), false, "Q@:");
    r.method(classes.heap, "usedSize", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) { s.x[0] = h.heap->used; }), false, "Q@:");
    r.method(classes.heap, "currentAllocatedSize", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) { s.x[0] = h.heap->size; }), false, "Q@:");
    r.method(classes.heap, "maxAvailableSizeWithAlignment:", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) {
        const auto free = h.heap->size - std::min<std::uint64_t>(h.heap->size, h.heap->used);
        s.x[0] = s.x[2] ? free / s.x[2] * s.x[2] : free;
    }), false, "Q@:Q");
    r.method(classes.heap, "storageMode", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(h.heap->options.storage);
    }), false, "Q@:");
    r.method(classes.heap, "cpuCacheMode", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) { s.x[0] = h.heap->options.cpu_cache_mode; }), false, "Q@:");
    r.method(classes.heap, "hazardTrackingMode", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) { s.x[0] = h.heap->options.hazard_tracking; }), false, "Q@:");
    r.method(classes.heap, "resourceOptions", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) { s.x[0] = h.heap->options.encode(); }), false, "Q@:");
    r.method(classes.heap, "type", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) { s.x[0] = h.heap->type; }), false, "q@:");
    r.method(classes.heap, "setPurgeableState:", [](CpuState& s, GuestMemory&) { s.x[0] = 2; }, false, "Q@:Q");
    r.method(classes.heap, "newBufferWithLength:options:", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) {
        if (!h.heap->reserve(s.x[2])) { s.x[0] = 0; return; }
        auto options = ResourceOptions::decode(s.x[3]);
        options.storage = h.heap->options.storage;
        const auto heap = h.heap;
        const auto length = s.x[2];
        s.x[0] = new_buffer(h.device, options, length, heap);
    }), false, "@@:QQ");
    r.method(classes.heap, "newTextureWithDescriptor:", on<HeapState>(heap_what, [](HeapState& h, CpuState& s, GuestMemory&) {
        const auto object = new_texture(h.device, s.x[2]);
        const auto texture = state<TextureState>(object, "MTLTexture");
        if (!h.heap->reserve(texture->texture->allocated_size())) { runtime().release(object); s.x[0] = 0; return; }
        texture->heap = h.heap;
        s.x[0] = object;
    }), false, "@@:@");

    // Shared events.
    const auto event_what = "MTLSharedEvent";
    r.method(classes.shared_event, "signaledValue", on<EventState>(event_what, [](EventState& e, CpuState& s, GuestMemory&) {
        s.x[0] = e.event->signaled_value();
    }), false, "Q@:");
    r.method(classes.shared_event, "setSignaledValue:", on<EventState>(event_what, [](EventState& e, CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        e.event->signal(s.x[2]);
        check_listeners(self);
    }), false, "v@:Q");
    r.method(classes.shared_event, "notifyListener:atValue:block:", on<EventState>(event_what, [](EventState&, CpuState& s, GuestMemory&) {
        add_listener(s.x[0], s.x[2], s.x[3], s.x[4]);
    }), false, "v@:@Q@?");
    r.method(classes.shared_event, "waitUntilSignaledValue:timeoutMS:", on<EventState>(event_what, [](EventState& e, CpuState& s, GuestMemory&) {
        const auto value = s.x[2];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.x[3]);
        const auto event = e.event;
        bool reached{};
        wait_for([&](std::uint64_t timeout) {
            reached = event->wait(value, timeout);
            return reached || std::chrono::steady_clock::now() >= deadline;
        }, "waitUntilSignaledValue:timeoutMS:");
        s.x[0] = reached;
    }), false, "B@:QQ");
    // newSharedEventHandle is g3's (shared_events.cpp).
    r.method(classes.listener, "init", [](CpuState& s, GuestMemory&) { static_cast<void>(runtime().objc.host_state<ListenerState>(s.x[0])); });
    r.method(classes.listener, "initWithDispatchQueue:", [&r](CpuState& s, GuestMemory&) {
        auto value = runtime().objc.host_state<ListenerState>(s.x[0]);
        value->queue = s.x[2];
        if (s.x[2]) value->keep(r.retain(s.x[2]));
    }, false, "@@:@");
    r.method(classes.listener, "dispatchQueue", [](CpuState& s, GuestMemory&) {
        const auto value = runtime().objc.find_host_state<ListenerState>(s.x[0]);
        s.x[0] = value ? value->queue : 0;
    });
    r.install_dealloc(classes.listener);

    // Capture: no capture tool exists; the manager answers that honestly.
    const auto manager = classes.capture_manager;
    r.method(manager, "sharedCaptureManager", [&r, manager](CpuState& s, GuestMemory&) {
        if (!capture_manager_object) {
            capture_manager_object = r.create(manager, std::make_shared<CaptureState>());
            if (const auto record = r.process.objects().find(capture_manager_object)) record->immortal = true;
        }
        s.x[0] = capture_manager_object;
    }, true);
    const auto new_scope = [&r](Address device, Address queue) {
        auto value = std::make_shared<ScopeState>();
        value->device = device;
        value->queue = queue;
        return r.create(classes.capture_scope, std::move(value));
    };
    r.method(manager, "newCaptureScopeWithDevice:", [new_scope](CpuState& s, GuestMemory&) { s.x[0] = new_scope(s.x[2], 0); }, false, "@@:@");
    r.method(manager, "newCaptureScopeWithCommandQueue:", [new_scope](CpuState& s, GuestMemory&) {
        const auto queue = state<QueueState>(s.x[2], "MTLCommandQueue");
        s.x[0] = new_scope(queue->device, s.x[2]);
    }, false, "@@:@");
    r.method(manager, "startCaptureWithDescriptor:error:", [&r](CpuState& s, GuestMemory& m) {
        // MTLCaptureErrorNotSupported (1).
        r.set_error(m, s.x[3], "MTLCaptureErrorDomain", 1, "GPU capture is not supported by Alchemy's Metal on Vulkan");
        s.x[0] = 0;
    }, false, "B@:@^@");
    for (const auto selector : {"startCaptureWithDevice:", "startCaptureWithCommandQueue:", "startCaptureWithScope:", "stopCapture"})
        r.method(manager, selector, [](CpuState&, GuestMemory&) {}, false, "v@:@");
    r.method(manager, "isCapturing", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    r.method(manager, "supportsDestination:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:q");
    r.method(manager, "defaultCaptureScope", on<CaptureState>("MTLCaptureManager", [](CaptureState& c, CpuState& s, GuestMemory&) {
        s.x[0] = c.default_scope;
    }));
    r.method(manager, "setDefaultCaptureScope:", on<CaptureState>("MTLCaptureManager", [&r](CaptureState& c, CpuState& s, GuestMemory&) {
        const auto previous = c.default_scope;
        c.default_scope = r.retain(s.x[2]);
        r.release(previous);
    }), false, "v@:@");
    const auto scope = classes.capture_scope;
    install_label(scope);
    install_device_getter(scope);
    r.method(scope, "beginScope", [](CpuState&, GuestMemory&) {}, false, "v@:");
    r.method(scope, "endScope", [](CpuState&, GuestMemory&) {}, false, "v@:");
    r.method(scope, "commandQueue", on<ScopeState>("MTLCaptureScope", [](ScopeState& value, CpuState& s, GuestMemory&) { s.x[0] = value.queue; }));

    // Binary archives: pipelines are cached by the device's persistent
    // VkPipelineCache when they are created, so adding them to an archive
    // and serializing it reduces to saving that cache.
    const auto archive = classes.archive;
    for (const auto selector : {"addComputePipelineFunctionsWithDescriptor:error:", "addRenderPipelineFunctionsWithDescriptor:error:",
                                "addTileRenderPipelineFunctionsWithDescriptor:error:", "addMeshRenderPipelineFunctionsWithDescriptor:error:"})
        r.method(archive, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "B@:@^@");
    r.method(archive, "addFunctionWithDescriptor:library:error:", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "B@:@@^@");
    r.method(archive, "serializeToURL:error:", on<ArchiveState>("MTLBinaryArchive", [](ArchiveState& a, CpuState& s, GuestMemory&) {
        device_of_object(a.device)->save_pipeline_cache();
        s.x[0] = 1;
    }), false, "B@:@^@");

    // Errors.
    const auto error = classes.error;
    const auto error_what = "NSError";
    r.method(error, "localizedDescription", on<ErrorState>(error_what, [&r](ErrorState& e, CpuState& s, GuestMemory&) {
        std::lock_guard lock(e.mutex);
        if (!e.label) e.label = r.cf.string(e.message);
        s.x[0] = e.label;
    }));
    r.method(error, "description", on<ErrorState>(error_what, [&r](ErrorState& e, CpuState& s, GuestMemory&) {
        s.x[0] = r.string("Error Domain=" + e.domain + " Code=" + std::to_string(e.code) + " \"" + e.message + "\"");
    }));
    r.method(error, "localizedFailureReason", on<ErrorState>(error_what, [&r](ErrorState& e, CpuState& s, GuestMemory&) {
        s.x[0] = r.string(e.message);
    }));
    r.method(error, "domain", on<ErrorState>(error_what, [&r](ErrorState& e, CpuState& s, GuestMemory&) { s.x[0] = r.string(e.domain); }));
    r.method(error, "code", on<ErrorState>(error_what, [](ErrorState& e, CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(e.code);
    }), false, "q@:");
    r.method(error, "userInfo", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
}

// ---- the device ----------------------------------------------------------------------------------

void install_device() {
    auto& r = runtime();
    const auto cls = classes.device;
    const auto what = "MTLDevice";
    const auto profile = [](Address object) -> const DeviceProfile& { return device_of_object(object)->profile(); };
    const auto answer = [&](std::string_view selector, auto function, const char* types = "Q@:") {
        r.method(cls, selector, [function, profile](CpuState& s, GuestMemory&) { s.x[0] = function(profile(s.x[0])); }, false, types);
    };
    r.method(cls, "name", on<DeviceState>(what, [](DeviceState& d, CpuState& s, GuestMemory&) { s.x[0] = d.name; }));
    answer("registryID", [](const DeviceProfile& p) { return p.registry_id; });
    answer("hasUnifiedMemory", [](const DeviceProfile& p) -> std::uint64_t { return p.unified_memory; }, "B@:");
    answer("isLowPower", [](const DeviceProfile& p) -> std::uint64_t { return p.low_power; }, "B@:");
    answer("isHeadless", [](const DeviceProfile& p) -> std::uint64_t { return p.headless; }, "B@:");
    answer("isRemovable", [](const DeviceProfile& p) -> std::uint64_t { return p.removable; }, "B@:");
    // MTLDeviceLocationBuiltIn (0) for an integrated GPU; Vulkan does not say
    // whether a discrete GPU sits in a slot or is external, so its location
    // is MTLDeviceLocationUnspecified (NSUIntegerMax).
    answer("location", [](const DeviceProfile& p) -> std::uint64_t { return p.low_power ? 0 : ~std::uint64_t{0}; });
    answer("locationNumber", [](const DeviceProfile&) -> std::uint64_t { return 0; });
    answer("maxTransferRate", [](const DeviceProfile&) -> std::uint64_t { return 0; });
    answer("peerGroupID", [](const DeviceProfile&) -> std::uint64_t { return 0; });
    answer("peerIndex", [](const DeviceProfile&) -> std::uint64_t { return 0; }, "I@:");
    answer("peerCount", [](const DeviceProfile&) -> std::uint64_t { return 0; }, "I@:");
    answer("recommendedMaxWorkingSetSize", [](const DeviceProfile& p) { return p.recommended_working_set; });
    answer("maxBufferLength", [](const DeviceProfile& p) { return p.max_buffer_length; });
    answer("maxThreadgroupMemoryLength", [](const DeviceProfile& p) -> std::uint64_t { return p.max_threadgroup_memory; });
    answer("maxArgumentBufferSamplerCount", [](const DeviceProfile& p) -> std::uint64_t { return p.argument_buffers_tier ? 2048 : 0; });
    answer("argumentBuffersSupport", [](const DeviceProfile& p) -> std::uint64_t { return p.argument_buffers_tier; });
    answer("readWriteTextureSupport", [](const DeviceProfile& p) -> std::uint64_t { return p.read_write_texture_tier; });
    answer("supportsShaderBarycentricCoordinates", [](const DeviceProfile& p) -> std::uint64_t { return p.barycentric_coordinates; }, "B@:");
    answer("areBarycentricCoordsSupported", [](const DeviceProfile& p) -> std::uint64_t { return p.barycentric_coordinates; }, "B@:");
    answer("supportsBCTextureCompression", [](const DeviceProfile& p) -> std::uint64_t { return p.bc_compression; }, "B@:");
    answer("supports32BitFloatFiltering", [](const DeviceProfile& p) -> std::uint64_t { return p.float32_filtering; }, "B@:");
    answer("supports32BitMSAA", [](const DeviceProfile& p) -> std::uint64_t { return (p.texture_sample_counts & 4) != 0; }, "B@:");
    answer("supportsQueryTextureLOD", [](const DeviceProfile& p) -> std::uint64_t { return p.query_texture_lod; }, "B@:");
    answer("isDepth24Stencil8PixelFormatSupported", [](const DeviceProfile& p) -> std::uint64_t { return p.depth24_stencil8; }, "B@:");
    // Not translated: no ray tracing, function pointers, dynamic libraries,
    // raster order groups, programmable sample positions or counters.
    for (const auto selector : {"supportsRaytracing", "supportsFunctionPointers", "supportsFunctionPointersFromRender",
                                "supportsRaytracingFromRender", "supportsPrimitiveMotionBlur", "supportsDynamicLibraries",
                                "supportsRenderDynamicLibraries", "areRasterOrderGroupsSupported",
                                "areProgrammableSamplePositionsSupported", "supportsPullModelInterpolation"})
        answer(selector, [](const DeviceProfile&) -> std::uint64_t { return 0; }, "B@:");
    // The timestamp counter set when Vulkan has graphics+compute timestamps,
    // sampled at stage boundaries only (MTLCounterSamplingPointAtStageBoundary,
    // 0): encoder starts and ends through sample attachments.
    r.method(cls, "supportsCounterSampling:", [profile](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] == 0 && profile(s.x[0]).timestamps;
    }, false, "B@:Q");
    r.method(cls, "counterSets", [&r, profile](CpuState& s, GuestMemory&) {
        std::vector<Address> sets;
        if (profile(s.x[0]).timestamps) sets.push_back(timestamp_set());
        s.x[0] = r.autorelease(r.cf.array(sets));
    });
    r.method(cls, "supportsVertexAmplificationCount:", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[2] <= 1; }, false, "B@:Q");
    r.method(cls, "supportsFamily:", [profile](CpuState& s, GuestMemory&) {
        s.x[0] = profile(s.x[0]).supports_family(static_cast<std::int64_t>(s.x[2]));
    }, false, "B@:q");
    r.method(cls, "supportsFeatureSet:", [profile](CpuState& s, GuestMemory&) {
        // MTLFeatureSet_macOS_GPUFamily1_v1..v4 (10000, 10001, 10003, 10004)
        // and GPUFamily2_v1 (10005) follow the Mac1 / Mac2 families.
        const auto& p = profile(s.x[0]);
        const auto set = s.x[2];
        s.x[0] = (set == 10000 || set == 10001 || set == 10003 || set == 10004) ? p.supports_family(gpu_family::mac1)
                 : set == 10005 ? p.supports_family(gpu_family::mac2) : 0;
    }, false, "B@:Q");
    r.method(cls, "supportsTextureSampleCount:", [profile](CpuState& s, GuestMemory&) {
        const auto count = s.x[2];
        s.x[0] = count == 1 || (count <= 64 && (count & (count - 1)) == 0 && (profile(s.x[0]).texture_sample_counts & count));
    }, false, "B@:Q");
    r.method(cls, "maxThreadsPerThreadgroup", [profile](CpuState& s, GuestMemory& m) {
        const auto& p = profile(s.x[0]);
        // MTLSize (24 bytes) returns through x8.
        for (int axis = 0; axis < 3; ++axis) m.write64(s.x[8] + 8 * axis, p.max_threads_per_threadgroup[axis]);
    }, false, "{MTLSize=QQQ}@:");
    r.method(cls, "currentAllocatedSize", [](CpuState& s, GuestMemory&) { s.x[0] = device_of_object(s.x[0])->allocated_bytes(); }, false, "Q@:");
    r.method(cls, "minimumLinearTextureAlignmentForPixelFormat:", [profile](CpuState& s, GuestMemory&) {
        s.x[0] = std::max<std::uint64_t>(16, profile(s.x[0]).min_texel_buffer_alignment);
    }, false, "Q@:Q");
    r.method(cls, "minimumTextureBufferAlignmentForPixelFormat:", [profile](CpuState& s, GuestMemory&) {
        s.x[0] = std::max<std::uint64_t>(16, profile(s.x[0]).min_texel_buffer_alignment);
    }, false, "Q@:Q");
    r.method(cls, "maximumConcurrentCompilationTaskCount", [](CpuState& s, GuestMemory&) {
        s.x[0] = std::max(1U, std::thread::hardware_concurrency());
    }, false, "Q@:");
    r.method(cls, "shouldMaximizeConcurrentCompilation", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "B@:");
    r.method(cls, "setShouldMaximizeConcurrentCompilation:", [](CpuState&, GuestMemory&) {}, false, "v@:B");
    // CPU time in mach_absolute_time's units (nanoseconds here) and GPU time
    // in timestamp-query ticks: the calibrated device clock when the driver
    // has VK_KHR/EXT_calibrated_timestamps, else the CPU time in ticks
    // (right rate, not phase-locked to the GPU's counter).
    r.method(cls, "sampleTimestamps:gpuTimestamp:", [](CpuState& s, GuestMemory& m) {
        const auto device = device_of_object(s.x[0]);
        const auto gpu = device->gpu_timestamp();
        const auto cpu = host::time::monotonic_ns();
        const auto period = device->profile().timestamp_period;
        if (s.x[2]) m.write64(s.x[2], cpu);
        if (s.x[3]) m.write64(s.x[3], gpu ? *gpu : static_cast<std::uint64_t>(static_cast<double>(cpu) / (period > 0 ? period : 1)));
    }, false, "v@:^Q^Q");
    r.method(cls, "heapBufferSizeAndAlignWithLength:options:", [](CpuState& s, GuestMemory&) {
        const auto length = s.x[2];
        s.x[0] = (length + 255) / 256 * 256;   // MTLSizeAndAlign {size, align} in x0/x1
        s.x[1] = 256;
    }, false, "{MTLSizeAndAlign=QQ}@:QQ");
    r.method(cls, "heapTextureSizeAndAlignWithDescriptor:", [](CpuState& s, GuestMemory&) {
        const auto d = texture_descriptor(s.x[2]);
        const auto* info = pixel_format_info(d.pixel_format);
        std::uint64_t size = 0;
        for (std::uint32_t level = 0; level < std::max(1U, d.mipmap_levels); ++level)
            size += static_cast<std::uint64_t>(std::max(1U, d.width >> level)) * std::max(1U, d.height >> level) *
                    std::max(1U, d.depth >> level) * (info ? info->block_bytes : 4);
        size *= std::max(1U, d.array_length) * std::max(1U, d.sample_count) * (d.type == TextureType::cube ? 6 : 1);
        s.x[0] = (size + 65535) / 65536 * 65536;
        s.x[1] = 65536;
    }, false, "{MTLSizeAndAlign=QQ}@:@");

    // Object creation.
    r.method(cls, "newCommandQueue", [&r](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<QueueState>();
        value->device = s.x[0];
        s.x[0] = r.create(classes.queue, std::move(value));
    });
    r.method(cls, "newCommandQueueWithMaxCommandBufferCount:", [&r](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<QueueState>();
        value->device = s.x[0];
        s.x[0] = r.create(classes.queue, std::move(value));
    }, false, "@@:Q");
    // MTLCommandQueueDescriptor (macOS 15): command buffers are not pooled
    // per queue, so maxCommandBufferCount does not bound them (as for
    // -newCommandQueueWithMaxCommandBufferCount:); a log state has no logs
    // to deliver (see -newLogStateWithDescriptor:error:).
    r.method(cls, "newCommandQueueWithDescriptor:", [&r](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        auto value = std::make_shared<QueueState>();
        value->device = s.x[0];
        s.x[0] = r.create(classes.queue, std::move(value));
    }, false, "@@:@");
    // MTLLogState (macOS 15) collects what shaders print with os_log. The
    // MSL frontend translates no shader logging, so a log state never has a
    // log: -addLogHandler: keeps nothing and no handler ever runs.
    // MTLLogStateErrorInvalidSize (1) for a negative buffer size.
    r.method(cls, "newLogStateWithDescriptor:error:", [&r](CpuState& s, GuestMemory& m) {
        const auto descriptor = s.x[2];
        if (!descriptor) { s.x[0] = 0; return; }
        auto value = std::make_shared<LogStateState>();
        value->device = s.x[0];
        value->level = static_cast<std::int64_t>(descriptor_u64(descriptor, "level"));
        value->buffer_size = static_cast<std::int64_t>(descriptor_u64(descriptor, "bufferSize"));
        if (value->buffer_size < 0) {
            r.set_error(m, s.x[3], "MTLLogStateErrorDomain", 1, "MTLLogStateDescriptor.bufferSize is negative");
            s.x[0] = 0;
            return;
        }
        s.x[0] = r.create(classes.log_state, std::move(value));
    }, false, "@@:@^@");
    r.method(cls, "newBufferWithLength:options:", [](CpuState& s, GuestMemory&) {
        s.x[0] = new_buffer(s.x[0], ResourceOptions::decode(s.x[3]), s.x[2]);
    }, false, "@@:QQ");
    r.method(cls, "newBufferWithBytes:length:options:", [](CpuState& s, GuestMemory& m) {
        const auto bytes = guest_bytes(m, s.x[2], s.x[3]);
        const auto object = new_buffer(s.x[0], ResourceOptions::decode(s.x[4]), s.x[3]);
        state<BufferState>(object, "MTLBuffer")->buffer->write(0, bytes);
        s.x[0] = object;
    }, false, "@@:^vQQ");
    // The client's memory as the buffer (Dawn's HostMappedPointer *(design)*):
    // imported, so CPU and GPU share the bytes. The deallocator block
    // (pointer, length) runs once the buffer is released and the GPU is done
    // with the memory, on the completion thread. A device that cannot import
    // the memory answers nil, Metal's failure value (reported once per reason).
    r.method(cls, "newBufferWithBytesNoCopy:length:options:deallocator:", [&r](CpuState& s, GuestMemory&) {
        const auto pointer = s.x[2], length = s.x[3], deallocator = s.x[5];
        const auto block = deallocator ? r.copy_block(deallocator) : 0;
        std::function<void()> released;
        if (block) released = [block, pointer, length] {
            try { run_block_later(block, {pointer, length}); } catch (...) {}   // the process is stopping
        };
        std::string why;
        auto buffer = Buffer::import_host(device_of_object(s.x[0]), reinterpret_cast<std::uint8_t*>(pointer), length,
                                          ResourceOptions::decode(s.x[4]), std::move(released), why);
        if (!buffer) {
            static std::mutex mutex;
            static std::set<std::string> reported;
            bool first{};
            { std::lock_guard lock(mutex); first = reported.insert(why).second; }
            if (first) std::fprintf(stderr, "[alchemy metal] newBufferWithBytesNoCopy:length:options:deallocator: answers nil: %s\n", why.c_str());
            if (block) static_cast<void>(r.call("__Block_release", {block}));
            s.x[0] = 0;
            return;
        }
        auto value = std::make_shared<BufferState>();
        value->buffer = std::move(buffer);
        value->device = s.x[0];
        s.x[0] = r.create(classes.buffer, std::move(value));
    }, false, "@@:^vQQ@?");
    r.method(cls, "newTextureWithDescriptor:", [](CpuState& s, GuestMemory&) { s.x[0] = new_texture(s.x[0], s.x[2]); }, false, "@@:@");
    // newTextureWithDescriptor:iosurface:plane: is g3's (surface_textures.cpp).
    r.method(cls, "newSamplerStateWithDescriptor:", [&r](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<SamplerObject>();
        value->sampler = Sampler::create(device_of_object(s.x[0]), sampler_descriptor(s.x[2]));
        value->device = s.x[0];
        s.x[0] = r.create(classes.sampler, std::move(value));
    }, false, "@@:@");
    r.method(cls, "newDepthStencilStateWithDescriptor:", [&r](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<DepthStencilObject>();
        value->state = std::make_shared<DepthStencilState>(depth_stencil(s.x[2]));
        value->device = s.x[0];
        s.x[0] = r.create(classes.depth_stencil, std::move(value));
    }, false, "@@:@");
    r.method(cls, "newHeapWithDescriptor:", [&r](CpuState& s, GuestMemory&) {
        const auto descriptor = s.x[2];
        const auto type = static_cast<std::uint32_t>(descriptor_u64(descriptor, "type"));
        if (type != 0) throw std::runtime_error("Alchemy Metal: placement and sparse MTLHeaps are not translated");
        auto value = std::make_shared<HeapState>();
        value->heap = std::make_shared<Heap>(device_of_object(s.x[0]), descriptor_u64(descriptor, "size"),
                                             ResourceOptions::decode(descriptor_u64(descriptor, "resourceOptions")), type);
        value->device = s.x[0];
        s.x[0] = r.create(classes.heap, std::move(value));
    }, false, "@@:@");
    r.method(cls, "newFence", [&r](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<FenceState>();
        value->device = s.x[0];
        s.x[0] = r.create(classes.fence, std::move(value));
    });
    const auto new_event = [&r](Address device, bool shared) {
        auto value = std::make_shared<EventState>();
        value->event = Event::create(device_of_object(device), shared);
        value->device = device;
        return r.create(shared ? classes.shared_event : classes.event, std::move(value));
    };
    r.method(cls, "newEvent", [new_event](CpuState& s, GuestMemory&) { s.x[0] = new_event(s.x[0], false); });
    r.method(cls, "newSharedEvent", [new_event](CpuState& s, GuestMemory&) { s.x[0] = new_event(s.x[0], true); });
    // newSharedEventWithHandle: is g3's (shared_events.cpp).
    r.method(cls, "newArgumentEncoderWithArguments:", [&r](CpuState& s, GuestMemory&) {
        auto value = layout_arguments(argument_entries(s.x[2]));
        value->device = s.x[0];
        s.x[0] = r.create(classes.argument_encoder, std::move(value));
    }, false, "@@:@");
    r.method(cls, "newBinaryArchiveWithDescriptor:error:", [&r](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<ArchiveState>();
        value->device = s.x[0];
        s.x[0] = r.create(classes.archive, std::move(value));
    }, false, "@@:@^@");
    // MTLCounterErrorDomain: MTLCounterErrorInvalid (2) for a counter set
    // other than the device's timestamp set, or no samples.
    r.method(cls, "newCounterSampleBufferWithDescriptor:error:", [&r, profile](CpuState& s, GuestMemory& m) {
        const auto descriptor = s.x[2];
        const auto set = descriptor ? descriptor_object(descriptor, "counterSet") : 0;
        const auto count = descriptor ? descriptor_u64(descriptor, "sampleCount") : 0;
        if (!profile(s.x[0]).timestamps || !set || set != timestamp_set_object) {
            r.set_error(m, s.x[3], "MTLCounterErrorDomain", 2, "the counter set is not one of this device's counterSets");
            s.x[0] = 0;
            return;
        }
        if (!count || count > 65536) {
            r.set_error(m, s.x[3], "MTLCounterErrorDomain", 2, "MTLCounterSampleBufferDescriptor.sampleCount must be 1..65536");
            s.x[0] = 0;
            return;
        }
        auto value = std::make_shared<SampleBufferState>();
        value->device = s.x[0];
        value->samples = CounterSampleBuffer::create(device_of_object(s.x[0]), static_cast<std::uint32_t>(count));
        if (const auto label = descriptor_object(descriptor, "label")) value->label = r.cf.copy(label);
        s.x[0] = r.create(classes.sample_buffer, std::move(value));
    }, false, "@@:@^@");
    r.method(cls, "newDefaultLibrary", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // A bundle's default.metallib is precompiled Apple GPU code, like
    // -newLibraryWithURL:error:.
    r.method(cls, "newDefaultLibraryWithBundle:error:", [&r](CpuState& s, GuestMemory& m) {
        r.set_error(m, s.x[3], "MTLLibraryErrorDomain", 1, "precompiled Metal libraries (default.metallib) are not translated");
        s.x[0] = 0;
    }, false, "@@:@^@");
    // A precompiled library handed over in memory (a dispatch_data_t; ANGLE's
    // built-in shaders): with ALCHEMY_MSL_DUMP set its bytes go to the dump
    // directory once (msl::dump_library_data; input for evaluating
    // translators, never for git). It then fails like the file forms below.
    r.method(cls, "newLibraryWithData:error:", [&r](CpuState& s, GuestMemory& m) {
        const auto data = s.x[2], error_out = s.x[3];
        if (data) {
            const auto out = r.call("_malloc", {16});
            if (out) {
                const auto map = r.call("_dispatch_data_create_map", {data, out, out + 8});
                const auto buffer = m.read64(out), size = m.read64(out + 8);
                if (map && buffer && size) {
                    std::string bytes(static_cast<std::size_t>(size), '\0');
                    m.read_bytes(buffer, std::span(reinterpret_cast<std::uint8_t*>(bytes.data()), bytes.size()));
                    static_cast<void>(msl::dump_library_data(bytes));
                }
                if (map) static_cast<void>(r.call("_dispatch_release", {map}));
                static_cast<void>(r.call("_free", {out}));
            }
        }
        // MTLLibraryErrorUnsupported (1): metallib binaries are Apple GPU code.
        r.set_error(m, error_out, "MTLLibraryErrorDomain", 1, "precompiled Metal libraries (metallib) are not translated");
        s.x[0] = 0;
    }, false, "@@:@^@");
    for (const auto selector : {"newLibraryWithURL:error:", "newLibraryWithFile:error:"})
        r.method(cls, selector, [&r](CpuState& s, GuestMemory& m) {
            // MTLLibraryErrorUnsupported (1): metallib binaries are Apple GPU code.
            r.set_error(m, s.x[3], "MTLLibraryErrorDomain", 1, "precompiled Metal libraries (metallib) are not translated");
            s.x[0] = 0;
        }, false, "@@:@^@");

    const auto compile = [&r](Address device, Address source, Address options, std::string& error) -> Address {
        perf::Scope timed(perf::mtl_library_ticks, perf::mtl_libraries);
        perf::phase("first newLibraryWithSource:");
        if (perf::enabled()) perf::count_key("newLibraryWithSource: by thread", perf::is_main_thread() ? "main" : "other");
        std::string text;
        msl::CompileOptions settings;
        {
            perf::Scope source_timed(perf::mtl_source_ticks);
            text = r.utf8(source);
            settings = compile_options(options);
        }
        settings.subgroup_size = device_of_object(device)->profile().thread_execution_width;
        static_cast<void>(msl::dump_source(text, settings));
        msl::CompileResult result;
        try {
            result = msl::compile(text, settings);
        } catch (const std::exception& failure) {
            error = std::string("Alchemy MSL compiler: ") + failure.what();
            return 0;
        }
        if (!result.success()) { error = msl::format_diagnostics(result.diagnostics); return 0; }
        auto value = std::make_shared<LibraryState>();
        value->library = std::make_shared<ShaderLibrary>(device_of_object(device), std::move(result.functions));
        value->device = device;
        return r.create(classes.library, std::move(value));
    };
    r.method(cls, "newLibraryWithSource:options:error:", [&r, compile](CpuState& s, GuestMemory& m) {
        std::string error;
        const auto error_out = s.x[4];
        s.x[0] = compile(s.x[0], s.x[2], s.x[3], error);
        if (!s.x[0]) r.set_error(m, error_out, "MTLLibraryErrorDomain", 3, error);   // MTLLibraryErrorCompileFailure
    }, false, "@@:@@^@");
    r.method(cls, "newLibraryWithSource:options:completionHandler:", [&r, compile](CpuState& s, GuestMemory&) {
        std::string error;
        const auto block = s.x[4];
        const auto library = compile(s.x[0], s.x[2], s.x[3], error);
        const auto failure = library ? 0 : r.error("MTLLibraryErrorDomain", 3, error);
        r.invoke_block(block, {library, failure}, false);
        r.release(library);
    }, false, "v@:@@@?");

    const auto render = [&r](CpuState& s, GuestMemory& m, Address reflection_out, Address error_out) {
        try {
            auto pipeline = render_pipeline(s.x[0], s.x[2]);
            if (reflection_out) m.write64(reflection_out, render_reflection(*pipeline));
            auto value = std::make_shared<RenderPipelineObject>();
            value->pipeline = std::move(pipeline);
            value->device = s.x[0];
            s.x[0] = r.create(classes.render_pipeline, std::move(value));
        } catch (const VulkanError&) {
            throw;
        } catch (const std::exception& failure) {
            r.set_error(m, error_out, "MTLLibraryErrorDomain", 2, failure.what());   // MTLLibraryErrorInternal
            if (reflection_out) m.write64(reflection_out, 0);
            s.x[0] = 0;
        }
    };
    r.method(cls, "newRenderPipelineStateWithDescriptor:error:", [render](CpuState& s, GuestMemory& m) {
        render(s, m, 0, s.x[3]);
    }, false, "@@:@^@");
    r.method(cls, "newRenderPipelineStateWithDescriptor:options:reflection:error:", [render](CpuState& s, GuestMemory& m) {
        render(s, m, s.x[4], s.x[5]);
    }, false, "@@:@Q^@^@");
    r.method(cls, "newRenderPipelineStateWithDescriptor:completionHandler:", [&r](CpuState& s, GuestMemory& m) {
        const auto block = s.x[3];
        auto copy = s;
        Address pipeline{};
        Address failure{};
        try {
            auto value = std::make_shared<RenderPipelineObject>();
            value->pipeline = render_pipeline(s.x[0], s.x[2]);
            value->device = s.x[0];
            pipeline = r.create(classes.render_pipeline, std::move(value));
        } catch (const VulkanError&) {
            throw;
        } catch (const std::exception& error) {
            failure = r.error("MTLLibraryErrorDomain", 2, error.what());
        }
        static_cast<void>(copy);
        static_cast<void>(m);
        r.invoke_block(block, {pipeline, failure}, false);
        r.release(pipeline);
    }, false, "v@:@@?");
    const auto compute = [&r](CpuState& s, GuestMemory& m, Address function, std::uint32_t max_threads, Address reflection_out,
                              Address error_out, const std::string& label) {
        try {
            auto pipeline = compute_pipeline(s.x[0], function, max_threads);
            pipeline->label = label;
            if (reflection_out) m.write64(reflection_out, compute_reflection(*pipeline));
            s.x[0] = new_compute_object(s.x[0], std::move(pipeline));
        } catch (const VulkanError&) {
            throw;
        } catch (const std::exception& failure) {
            r.set_error(m, error_out, "MTLLibraryErrorDomain", 2, failure.what());
            if (reflection_out) m.write64(reflection_out, 0);
            s.x[0] = 0;
        }
    };
    r.method(cls, "newComputePipelineStateWithFunction:error:", [compute](CpuState& s, GuestMemory& m) {
        compute(s, m, s.x[2], 0, 0, s.x[3], {});
    }, false, "@@:@^@");
    r.method(cls, "newComputePipelineStateWithFunction:options:reflection:error:", [compute](CpuState& s, GuestMemory& m) {
        compute(s, m, s.x[2], 0, s.x[4], s.x[5], {});
    }, false, "@@:@Q^@^@");
    r.method(cls, "newComputePipelineStateWithDescriptor:options:reflection:error:", [&r, compute](CpuState& s, GuestMemory& m) {
        const auto descriptor = s.x[2];
        compute(s, m, descriptor_object(descriptor, "computeFunction"),
                static_cast<std::uint32_t>(descriptor_u64(descriptor, "maxTotalThreadsPerThreadgroup")), s.x[4], s.x[5],
                r.utf8(descriptor_object(descriptor, "label")));
    }, false, "@@:@Q^@^@");
    r.method(cls, "newComputePipelineStateWithDescriptor:options:completionHandler:", [&r](CpuState& s, GuestMemory&) {
        // The handler runs before this returns (an allowed schedule: Metal
        // promises only that it runs once creation finished).
        const auto descriptor = s.x[2];
        const auto block = s.x[4];
        const auto options = s.x[3];
        Address pipeline{}, reflection{}, failure{};
        try {
            auto value = compute_pipeline(s.x[0], descriptor_object(descriptor, "computeFunction"),
                                          static_cast<std::uint32_t>(descriptor_u64(descriptor, "maxTotalThreadsPerThreadgroup")));
            value->label = r.utf8(descriptor_object(descriptor, "label"));
            if (options) reflection = compute_reflection(*value);
            pipeline = new_compute_object(s.x[0], std::move(value));
        } catch (const VulkanError&) {
            throw;
        } catch (const std::exception& error) {
            failure = r.error("MTLLibraryErrorDomain", 2, error.what());
        }
        r.invoke_block(block, {pipeline, reflection, failure}, false);
        r.release(pipeline);
    }, false, "v@:@Q@?");
    r.method(cls, "newComputePipelineStateWithFunction:completionHandler:", [&r](CpuState& s, GuestMemory&) {
        const auto block = s.x[3];
        Address pipeline{}, failure{};
        try {
            pipeline = new_compute_object(s.x[0], compute_pipeline(s.x[0], s.x[2], 0));
        } catch (const VulkanError&) {
            throw;
        } catch (const std::exception& error) {
            failure = r.error("MTLLibraryErrorDomain", 2, error.what());
        }
        r.invoke_block(block, {pipeline, failure}, false);
        r.release(pipeline);
    }, false, "v@:@@?");
}

} // namespace

Address Runtime::error(std::string_view domain, std::int64_t code, std::string_view message) {
    auto value = std::make_shared<ErrorState>();
    value->domain = domain;
    value->code = code;
    value->message = message;
    return autorelease(create(classes.error, std::move(value)));
}

void Runtime::set_error(GuestMemory& memory, Address out, std::string_view domain, std::int64_t code,
                        std::string_view message) {
    if (ObjcRuntime::tracing()) std::cerr << "[metal] " << domain << " " << code << ": " << message << '\n';
    if (out) memory.write64(out, error(domain, code, message));
}

std::shared_ptr<Event> event_of(Address object) { return state<EventState>(object, "MTLEvent")->event; }

// MTLCommonCounterSet and MTLCommonCounter names (NSString constants). The
// device's timestamp set and its counter answer these names (Dawn matches
// them case-insensitively). Their string values are not in Apple's headers
// (unverified). Clients compare a set's or counter's name with these same
// exported constants (Dawn's IsGPUCounterSupported, by design), so what
// matters is that -name answers the constant's value, which the tests pin;
// only code that hard-codes Apple's literal text would see a difference.
void install_counter_names(Runtime& r, const std::source_location& where = std::source_location::current()) {
    registration_audit::Site site(where);
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_MTLCommonCounterSetTimestamp", "timestamp"},
             {"_MTLCommonCounterSetStageUtilization", "stageutilization"},
             {"_MTLCommonCounterSetStatistic", "statistic"},
             {"_MTLCommonCounterTimestamp", "GPUTimestamp"}}) {
        const auto slot = r.process.allocate_guest(8);
        r.process.main_engine().write64(slot, r.cf.constant_string(value));
        r.process.install_export(std::string(metal_library), symbol, {slot, ExportKind::data});
    }
}

namespace {

// The timestamp MTLCounterSet ({MTLCommonCounterTimestamp}): one immortal
// object, shared by every device of the process.
Address timestamp_set() {
    auto& r = runtime();
    std::lock_guard lock(device_mutex);
    if (!timestamp_set_object) {
        timestamp_set_object = r.create(classes.counter_set, std::make_shared<Held>());
        if (const auto record = r.process.objects().find(timestamp_set_object)) record->immortal = true;
    }
    return timestamp_set_object;
}

void install_counters(Runtime& r) {
    const auto make = [&](std::string_view name, std::string_view protocol) {
        const auto cls = r.define_class(name, "NSObject", false);
        r.objc.add_native_protocol(cls, protocol);
        r.install_dealloc(cls);
        return cls;
    };
    classes.counter_set = make("AlchemyMTLCounterSet", "MTLCounterSet");
    classes.counter = make("AlchemyMTLCounter", "MTLCounter");
    classes.sample_buffer = make("AlchemyMTLCounterSampleBuffer", "MTLCounterSampleBuffer");
    const auto set_name = r.cf.constant_string("timestamp"), counter_name = r.cf.constant_string("GPUTimestamp");
    // The counters array is made on first use (no objects at install time).
    r.method(classes.counter_set, "name", [set_name](CpuState& s, GuestMemory&) { s.x[0] = set_name; }, false, "@@:");
    r.method(classes.counter_set, "counters", [&r](CpuState& s, GuestMemory&) {
        static Address counters{};
        std::lock_guard lock(device_mutex);
        if (!counters) {
            const auto counter = r.create(classes.counter, std::make_shared<Held>());
            if (const auto record = r.process.objects().find(counter)) record->immortal = true;
            const std::array<Address, 1> items{counter};
            counters = r.cf.array(items);
            if (const auto record = r.process.objects().find(counters)) record->immortal = true;
        }
        s.x[0] = counters;
    }, false, "@@:");
    r.method(classes.counter, "name", [counter_name](CpuState& s, GuestMemory&) { s.x[0] = counter_name; }, false, "@@:");
    const auto buffer = classes.sample_buffer;
    install_label(buffer);
    install_device_getter(buffer);
    const auto what = "MTLCounterSampleBuffer";
    r.method(buffer, "sampleCount", on<SampleBufferState>(what, [](SampleBufferState& b, CpuState& s, GuestMemory&) {
        s.x[0] = b.samples->count;
    }), false, "Q@:");
    // NSData of MTLCounterResultTimestamp (8 bytes per sample); NSRange in x2/x3.
    r.method(buffer, "resolveCounterRange:", on<SampleBufferState>(what, [&r](SampleBufferState& b, CpuState& s, GuestMemory& m) {
        if (s.x[2] > UINT32_MAX || s.x[3] > UINT32_MAX) { s.x[0] = 0; return; }
        const auto values = b.samples->resolve(static_cast<std::uint32_t>(s.x[2]), static_cast<std::uint32_t>(s.x[3]));
        const auto bytes = values.size() * 8;
        const auto scratch = r.process.allocate_guest(std::max<std::size_t>(bytes, 8));
        m.write_bytes(scratch, std::span(reinterpret_cast<const std::uint8_t*>(values.data()), bytes));
        const std::array<std::uint64_t, 3> arguments{0, scratch, bytes};
        const auto data = r.cf.invoke("CFDataCreate", arguments);
        r.process.release_guest(scratch);
        s.x[0] = r.autorelease(data);
    }), false, "@@:{_NSRange=QQ}");
}

} // namespace

// ---- installation ------------------------------------------------------------------------------------

void install_objects(Runtime& r) {
    // Concrete classes: project-authored names, Metal protocols adopted.
    const auto make = [&](std::string_view name, std::initializer_list<std::string_view> protocols) {
        const auto cls = r.define_class(name, "NSObject", false);
        for (const auto protocol : protocols) r.objc.add_native_protocol(cls, protocol);
        r.install_dealloc(cls);
        return cls;
    };
    classes.device = make("AlchemyMTLDevice", {"MTLDevice"});
    classes.buffer = make("AlchemyMTLBuffer", {"MTLBuffer", "MTLResource", "MTLAllocation"});
    classes.texture = make("AlchemyMTLTexture", {"MTLTexture", "MTLResource", "MTLAllocation"});
    classes.sampler = make("AlchemyMTLSamplerState", {"MTLSamplerState"});
    classes.depth_stencil = make("AlchemyMTLDepthStencilState", {"MTLDepthStencilState"});
    classes.heap = make("AlchemyMTLHeap", {"MTLHeap", "MTLAllocation"});
    classes.library = make("AlchemyMTLLibrary", {"MTLLibrary"});
    classes.function = r.define_class("AlchemyMTLFunction", "NSObject", false);
    r.objc.add_native_protocol(classes.function, "MTLFunction");
    classes.render_pipeline = make("AlchemyMTLRenderPipelineState", {"MTLRenderPipelineState"});
    classes.compute_pipeline = make("AlchemyMTLComputePipelineState", {"MTLComputePipelineState"});
    classes.queue = make("AlchemyMTLCommandQueue", {"MTLCommandQueue"});
    classes.command_buffer = make("AlchemyMTLCommandBuffer", {"MTLCommandBuffer"});
    classes.render_encoder = make("AlchemyMTLRenderCommandEncoder", {"MTLRenderCommandEncoder", "MTLCommandEncoder"});
    classes.compute_encoder = make("AlchemyMTLComputeCommandEncoder", {"MTLComputeCommandEncoder", "MTLCommandEncoder"});
    classes.blit_encoder = make("AlchemyMTLBlitCommandEncoder", {"MTLBlitCommandEncoder", "MTLCommandEncoder"});
    classes.argument_encoder = make("AlchemyMTLArgumentEncoder", {"MTLArgumentEncoder"});
    classes.fence = make("AlchemyMTLFence", {"MTLFence"});
    classes.event = make("AlchemyMTLEvent", {"MTLEvent"});
    classes.shared_event = r.define_class("AlchemyMTLSharedEvent", "AlchemyMTLEvent", false);
    r.objc.add_native_protocol(classes.shared_event, "MTLSharedEvent");
    r.objc.add_native_protocol(classes.shared_event, "MTLEvent");
    classes.listener = r.define_class("MTLSharedEventListener");
    classes.capture_manager = r.define_class("MTLCaptureManager");
    classes.capture_scope = make("AlchemyMTLCaptureScope", {"MTLCaptureScope"});
    classes.archive = make("AlchemyMTLBinaryArchive", {"MTLBinaryArchive"});
    classes.error = make("AlchemyMTLError", {});
    classes.argument = make("AlchemyMTLArgument", {});
    classes.render_reflection = make("AlchemyMTLRenderPipelineReflection", {});
    classes.compute_reflection = make("AlchemyMTLComputePipelineReflection", {});
    classes.log_state = make("AlchemyMTLLogState", {"MTLLogState"});
    install_counters(r);
    r.method(classes.log_state, "addLogHandler:", [](CpuState& s, GuestMemory&) {
        static_cast<void>(state<LogStateState>(s.x[0], "MTLLogState"));
    }, false, "v@:@?");
    install_device_getter(classes.log_state);
    install_counter_names(r);
    install_label(classes.device);
    install_device();
    install_buffer();
    install_texture();
    install_surface_textures(r);   // g3: IOSurface-backed textures (surface_textures.cpp)
    install_library_and_function();
    install_pipeline_states();
    install_argument_encoder();
    install_queue();
    install_command_buffer();
    install_render_encoder();
    install_compute_encoder();
    install_blit_encoder();
    install_small_objects();
    install_shared_events(r);   // g3: cross-process MTLSharedEvent (shared_events.cpp)
    install_listener_dispatch(r);

    // MTLCreateSystemDefaultDevice / MTLCopyAllDevices replace the headless
    // shells; the Vulkan device is created on the first call.
    static_cast<void>(r.process.install_function("_MTLCreateSystemDefaultDevice", [](CpuState& s, GuestMemory&) {
        s.x[0] = system_device_object();
    }, std::string(metal_library)));
    static_cast<void>(r.process.install_function("_MTLCopyAllDevices", [&r](CpuState& s, GuestMemory&) {
        std::vector<Address> devices;
        if (const auto device = system_device_object()) devices.push_back(device);
        s.x[0] = r.cf.array(devices);
    }, std::string(metal_library)));
}

Address system_device_object() {
    std::lock_guard lock(device_mutex);
    if (device_object) return device_object;
    auto& r = runtime();
    std::shared_ptr<Device> device;
    try {
        device = system_device();
    } catch (const std::exception& failure) {
        // MTLCreateSystemDefaultDevice's documented failure value is nil.
        if (!device_failure_reported) std::cerr << "Alchemy Metal: no Metal device: " << failure.what() << '\n';
        device_failure_reported = true;
        return 0;
    }
    auto value = std::make_shared<DeviceState>();
    value->device = device;
    value->name = r.cf.string(device->profile().name);
    if (const auto record = r.process.objects().find(value->name)) record->immortal = true;
    device_object = r.create(classes.device, std::move(value));
    if (const auto record = r.process.objects().find(device_object)) record->immortal = true;
    publish_to_quartzcore(device_object, device);
    return device_object;
}

// ---- interop (interop.hpp) ------------------------------------------------------------------------------

} // namespace alchemy::metal::bridge

namespace alchemy::metal {

std::shared_ptr<Device> device_of(std::uint64_t object) {
    const auto value = bridge::optional_state<bridge::DeviceState>(object);
    return value ? value->device : nullptr;
}

std::uint64_t new_texture_object(std::shared_ptr<Texture> texture) {
    const auto device = bridge::system_device_object();
    if (!device) throw std::runtime_error("Alchemy Metal: no Metal device for a drawable texture");
    return bridge::new_texture_object_for(device, std::move(texture));
}

std::shared_ptr<Texture> texture_of(std::uint64_t object) {
    const auto value = bridge::optional_state<bridge::TextureState>(object);
    return value ? value->texture : nullptr;
}

} // namespace alchemy::metal
