// Guest-visible Metal objects (darwin/metal/metal_objects.cpp,
// metal_descriptors.cpp) through the Objective-C runtime, as Chrome's GPU
// process (Dawn, ANGLE) uses them:
//   - the macOS 15 descriptors (MTLCommandQueueDescriptor,
//     MTLLogStateDescriptor) and their defaults, and the counter names;
//   - with a Metal device (skipped without a Vulkan 1.3 GPU): the device's
//     identity agrees with the IORegistry accelerator entry and
//     metal::gpu_identities(); the timestamp counter set (stage-boundary
//     samples, resolveCounters:, resolveCounterRange:);
//     -newCommandQueueWithDescriptor:, -newLogStateWithDescriptor:error:;
//     MTLSharedEvent CPU signals and waits, GPU-side waits for a CPU signal
//     and GPU signals; MTLEvent signal-then-wait across command buffers;
//     MTLFence updates and waits between encoders.
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/metal/gpu_identity.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <chrono>
#include <thread>

namespace {
using namespace fill_test;

constexpr std::string_view metal = "/System/Library/Frameworks/Metal.framework/Versions/A/Metal";
constexpr std::string_view iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";
constexpr std::string_view cf_library = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

Address make(Fixture& f, std::string_view cls) { return f.send(f.send(f.cls(cls), "alloc"), "init"); }

void descriptors(Fixture& f) {
    const auto queue = make(f, "MTLCommandQueueDescriptor");
    require(f.send(queue, "maxCommandBufferCount") == 64, "maxCommandBufferCount default");
    require(f.send(queue, "logState") == 0, "logState default");
    static_cast<void>(f.send(queue, "setMaxCommandBufferCount:", {8}));
    const auto copy = f.send(queue, "copy");
    require(copy != queue && f.send(copy, "maxCommandBufferCount") == 8, "queue descriptor copies");
    const auto log = make(f, "MTLLogStateDescriptor");
    require(f.send(log, "level") == 0 && f.send(log, "bufferSize") == 0, "log state descriptor defaults");
    static_cast<void>(f.send(log, "setLevel:", {4}));   // MTLLogLevelError
    require(f.send(log, "level") == 4, "level");
}

void counter_names(Fixture& f) {
    require(f.string_constant(metal, "_MTLCommonCounterSetTimestamp") == "timestamp", "MTLCommonCounterSetTimestamp");
    require(f.string_constant(metal, "_MTLCommonCounterTimestamp") == "GPUTimestamp", "MTLCommonCounterTimestamp");
    require(f.string_constant(metal, "_MTLCommonCounterSetStatistic") == "statistic", "MTLCommonCounterSetStatistic");
    require(f.string_constant(metal, "_MTLCommonCounterSetStageUtilization") == "stageutilization", "MTLCommonCounterSetStageUtilization");
}

Address device(Fixture& f) {
    static Address value = f.call(metal, "_MTLCreateSystemDefaultDevice");
    return value;
}

void identity(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  no Metal device (no Vulkan 1.3 GPU): skipped\n"; return; }
    const auto registry = f.send(d, "registryID");
    const auto gpus = alchemy::metal::gpu_identities();
    const auto chosen = std::find_if(gpus.begin(), gpus.end(), [](const auto& gpu) { return gpu.metal; });
    require(chosen != gpus.end(), "gpu_identities marks the Metal device");
    require(std::count_if(gpus.begin(), gpus.end(), [](const auto& gpu) { return gpu.metal; }) == 1, "exactly one Metal device");
    require(chosen->registry_id == registry, "identity registry ID == -[MTLDevice registryID]");
    require(f.cf.utf8(f.send(d, "name")) == chosen->name, "identity name == -[MTLDevice name]");
    // Dawn's GetDeviceIORegistryPCIInfo.
    const auto accelerator = f.call(iokit, "_IOServiceGetMatchingService", {0, f.call(iokit, "_IORegistryEntryIDMatching", {registry})});
    require(accelerator != 0, "the IORegistry has the device's accelerator");
    const auto parent = f.block(4);
    require(f.status(iokit, "_IORegistryEntryGetParentEntry", {accelerator, f.text("IOService"), parent}) == 0, "accelerator parent");
    const auto vendor = f.call(iokit, "_IORegistryEntrySearchCFProperty", {f.engine.read32(parent), f.text("IOService"),
                                                                          f.cf.string("vendor-id"), 0, 3});
    require(vendor && f.engine.read32(f.call(cf_library, "_CFDataGetBytePtr", {vendor})) == chosen->vendor_id, "vendor-id");
    // Properties Dawn and ANGLE read.
    require(f.send(d, "hasUnifiedMemory") == (chosen->integrated || chosen->cpu ? 1U : 0U), "hasUnifiedMemory");
    require(f.send(d, "recommendedMaxWorkingSetSize") == chosen->device_local_bytes / 4 * 3, "recommendedMaxWorkingSetSize");
    require(f.send(d, "peerGroupID") == 0 && f.send(d, "peerCount") == 0, "no peer group");
    const auto location = f.send(d, "location");
    require(chosen->integrated ? location == 0 : location == ~0ULL, "location: built-in when integrated, else unspecified");
    require(f.send(d, "supportsFamily:", {3001}) == 1, "MTLGPUFamilyCommon1");
    require(f.send(d, "supportsFamily:", {1001}) == 0, "no Apple families");
    const auto cpu = f.block(8), gpu = f.block(8);
    static_cast<void>(f.send(d, "sampleTimestamps:gpuTimestamp:", {cpu, gpu}));
    require(f.engine.read64(cpu) != 0 && f.engine.read64(gpu) != 0, "sampleTimestamps:gpuTimestamp:");
    const auto cpu_first = f.engine.read64(cpu), gpu_first = f.engine.read64(gpu);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    static_cast<void>(f.send(d, "sampleTimestamps:gpuTimestamp:", {cpu, gpu}));
    require(f.engine.read64(cpu) > cpu_first && f.engine.read64(gpu) > gpu_first, "both clocks advance");
}

// The timestamp counter set at stage boundaries: encoder start/end samples
// through pass descriptors, resolveCounters: and resolveCounterRange:.
void counters(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  skipped\n"; return; }
    const auto sets = f.send(d, "counterSets");
    for (Address point = 1; point <= 4; ++point) require(f.send(d, "supportsCounterSampling:", {point}) == 0, "only stage-boundary sampling");
    if (f.cf.length(sets) == 0) {
        require(f.send(d, "supportsCounterSampling:", {0}) == 0, "no timestamps, no sampling");
        std::cout << "  no timestamp counter set on this GPU\n";
        return;
    }
    require(f.cf.length(sets) == 1 && f.send(d, "supportsCounterSampling:", {0}) == 1, "one counter set, stage-boundary sampling");
    const auto set = f.cf.element(sets, 0);
    require(f.cf.utf8(f.send(set, "name")) == f.string_constant(metal, "_MTLCommonCounterSetTimestamp"), "the timestamp set");
    const auto counters = f.send(set, "counters");
    require(f.cf.length(counters) == 1 &&
            f.cf.utf8(f.send(f.cf.element(counters, 0), "name")) == f.string_constant(metal, "_MTLCommonCounterTimestamp"), "GPUTimestamp");
    const auto descriptor = make(f, "MTLCounterSampleBufferDescriptor");
    static_cast<void>(f.send(descriptor, "setCounterSet:", {set}));
    static_cast<void>(f.send(descriptor, "setSampleCount:", {4}));
    const auto error = f.block(8);
    const auto samples = f.send(d, "newCounterSampleBufferWithDescriptor:error:", {descriptor, error});
    require(samples && f.send(samples, "sampleCount") == 4 && f.send(samples, "device") == d, "newCounterSampleBufferWithDescriptor:");
    // A compute pass sampled at its start and end, then a blit resolving into a buffer.
    const auto pass = f.send(f.cls("MTLComputePassDescriptor"), "computePassDescriptor");
    const auto attachment = f.send(f.send(pass, "sampleBufferAttachments"), "objectAtIndexedSubscript:", {0});
    static_cast<void>(f.send(attachment, "setSampleBuffer:", {samples}));
    static_cast<void>(f.send(attachment, "setStartOfEncoderSampleIndex:", {0}));
    static_cast<void>(f.send(attachment, "setEndOfEncoderSampleIndex:", {1}));
    const auto destination = f.send(d, "newBufferWithLength:options:", {64, 0});
    const auto queue = f.send(d, "newCommandQueue");
    const auto commands = f.send(queue, "commandBuffer");
    const auto compute = f.send(commands, "computeCommandEncoderWithDescriptor:", {pass});
    static_cast<void>(f.send(compute, "endEncoding"));
    const auto blit = f.send(commands, "blitCommandEncoder");
    alchemy::CallRegs resolve{};
    resolve.x[2] = samples; resolve.x[3] = 0; resolve.x[4] = 4; resolve.x[5] = destination; resolve.x[6] = 0;
    static_cast<void>(f.objc.send(blit, "resolveCounters:inRange:destinationBuffer:destinationOffset:", resolve));
    static_cast<void>(f.send(blit, "endEncoding"));
    static_cast<void>(f.send(commands, "commit"));
    static_cast<void>(f.send(commands, "waitUntilCompleted"));
    require(f.send(commands, "status") == 4, "the sampled command buffer completes");
    const auto contents = f.send(destination, "contents");
    const auto start = f.engine.read64(contents), end = f.engine.read64(contents + 8);
    require(start != ~0ULL && end != ~0ULL && start <= end, "start and end timestamps");
    require(f.engine.read64(contents + 16) == ~0ULL && f.engine.read64(contents + 24) == ~0ULL, "unwritten samples are MTLCounterErrorValue");
    const auto data = f.send(samples, "resolveCounterRange:", {0, 2});
    require(data && f.call(cf_library, "_CFDataGetLength", {data}) == 16, "resolveCounterRange: NSData");
    const auto bytes = f.call(cf_library, "_CFDataGetBytePtr", {data});
    require(f.engine.read64(bytes) == start && f.engine.read64(bytes + 8) == end, "the CPU resolve agrees");
    // Another counter set is refused.
    static_cast<void>(f.send(descriptor, "setCounterSet:", {0}));
    require(f.send(d, "newCounterSampleBufferWithDescriptor:error:", {descriptor, error}) == 0 &&
            f.send(f.engine.read64(error), "code") == 2, "MTLCounterErrorInvalid without the timestamp set");
}

// setSamplerState:lodMinClamp:lodMaxClamp:atIndex: and the array form on
// every encoder stage (floats in s0/s1; the Vulkan test checks the sampling).
void sampler_lod_clamps(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  skipped\n"; return; }
    const auto render = f.cls("AlchemyMTLRenderCommandEncoder"), compute_class = f.cls("AlchemyMTLComputeCommandEncoder");
    for (const auto* selector : {"setVertexSamplerState:lodMinClamp:lodMaxClamp:atIndex:", "setFragmentSamplerState:lodMinClamp:lodMaxClamp:atIndex:",
                                 "setVertexSamplerStates:lodMinClamps:lodMaxClamps:withRange:",
                                 "setFragmentSamplerStates:lodMinClamps:lodMaxClamps:withRange:"})
        require(f.send(render, "instancesRespondToSelector:", {f.objc.selector(selector)}) == 1, selector);
    const auto descriptor = make(f, "MTLSamplerDescriptor");
    static_cast<void>(f.send(descriptor, "setMipFilter:", {1}));
    const auto sampler = f.send(d, "newSamplerStateWithDescriptor:", {descriptor});
    require(sampler != 0, "newSamplerStateWithDescriptor:");
    const auto queue = f.send(d, "newCommandQueue");
    const auto commands = f.send(queue, "commandBuffer");
    const auto compute = f.send(commands, "computeCommandEncoder");
    require(f.send(compute_class, "instancesRespondToSelector:", {f.objc.selector("setSamplerState:lodMinClamp:lodMaxClamp:atIndex:")}) == 1,
            "compute setSamplerState:lodMinClamp:lodMaxClamp:atIndex:");
    alchemy::CallRegs one{};
    one.x[2] = sampler; one.x[3] = 3; one.set_s(0, 1.0F); one.set_s(1, 4.0F);
    static_cast<void>(f.objc.send(compute, "setSamplerState:lodMinClamp:lodMaxClamp:atIndex:", one));
    const auto samplers = f.block(16), mins = f.block(8), maxes = f.block(8);
    f.engine.write64(samplers, sampler); f.engine.write64(samplers + 8, sampler);
    f.engine.write32(mins, std::bit_cast<std::uint32_t>(0.0F)); f.engine.write32(mins + 4, std::bit_cast<std::uint32_t>(2.0F));
    f.engine.write32(maxes, std::bit_cast<std::uint32_t>(1.0F)); f.engine.write32(maxes + 4, std::bit_cast<std::uint32_t>(8.0F));
    alchemy::CallRegs many{};
    many.x[2] = samplers; many.x[3] = mins; many.x[4] = maxes; many.x[5] = 4; many.x[6] = 2;
    static_cast<void>(f.objc.send(compute, "setSamplerStates:lodMinClamps:lodMaxClamps:withRange:", many));
    static_cast<void>(f.send(compute, "endEncoding"));
    static_cast<void>(f.send(commands, "commit"));
    static_cast<void>(f.send(commands, "waitUntilCompleted"));
    require(f.send(commands, "status") == 4, "the command buffer with clamped samplers completes");
}

// A compiler-emitted (global) block whose invoke runs `handler`.
Address global_block(Fixture& f, std::string name, transmute::GuestCallHandler handler) {
    const auto invoke = f.process.install_function(std::move(name), std::move(handler), "alchemy.test");
    const auto value = f.process.allocate_guest(32), descriptor = f.process.allocate_guest(16);
    f.engine.write64(descriptor, 0);
    f.engine.write64(descriptor + 8, 32);
    f.engine.write64(value, f.symbol("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock"));
    f.engine.write32(value + 8, 1U << 28);   // BLOCK_IS_GLOBAL
    f.engine.write32(value + 12, 0);
    f.engine.write64(value + 16, invoke);
    f.engine.write64(value + 24, descriptor);
    return value;
}

// -[MTLSharedEvent notifyListener:atValue:block:]: a listener made with a
// dispatch queue runs the block on that queue (dispatch_get_specific sees
// the queue's key there); one without a queue still runs it; a value already
// reached notifies at once.
void shared_event_listeners(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  skipped\n"; return; }
    constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";
    const auto key = f.block(8), marker = f.block(8);
    const auto queue = f.call(sys, "_dispatch_queue_create", {f.text("alchemy.test.listener"), 0});
    f.call(sys, "_dispatch_queue_set_specific", {queue, key, marker, 0});
    const auto get_specific = f.symbol(sys, "_dispatch_get_specific");
    struct Seen { std::atomic<int> calls{}; std::atomic<std::uint64_t> event{}, value{}, specific{}; };
    auto queued = std::make_shared<Seen>(), plain = std::make_shared<Seen>();
    auto& process = f.process;
    const auto record = [&process, key, get_specific](std::shared_ptr<Seen> seen) {
        return [&process, key, get_specific, seen](transmute::CpuState& s, transmute::GuestMemory&) {
            seen->event = s.x[1];
            seen->value = s.x[2];
            const std::array<std::uint64_t, 1> arguments{key};
            seen->specific = alchemy::EngineAdapter(process.calling_engine()).call(get_specific, arguments);
            ++seen->calls;
        };
    };
    const auto on_queue = global_block(f, "metal_test_listener_queued", record(queued));
    const auto off_queue = global_block(f, "metal_test_listener_plain", record(plain));
    const auto with_queue = f.send(f.send(f.cls("MTLSharedEventListener"), "alloc"), "initWithDispatchQueue:", {queue});
    require(f.send(with_queue, "dispatchQueue") == queue, "the listener keeps its queue");
    const auto without_queue = make(f, "MTLSharedEventListener");
    const auto event = f.send(d, "newSharedEvent");
    static_cast<void>(f.send(event, "notifyListener:atValue:block:", {with_queue, 3, on_queue}));
    static_cast<void>(f.send(event, "notifyListener:atValue:block:", {without_queue, 2, off_queue}));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(queued->calls == 0 && plain->calls == 0, "no notification before the value");
    static_cast<void>(f.send(event, "setSignaledValue:", {3}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((queued->calls == 0 || plain->calls == 0) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(queued->calls == 1 && queued->event == event && queued->value == 3, "the queued listener's block ran once");
    require(queued->specific == marker, "the block ran on the listener's dispatch queue");
    require(plain->calls == 1 && plain->event == event && plain->value == 2, "a listener without a queue still runs");
    // Already reached: notified at once (asynchronously, on the queue).
    static_cast<void>(f.send(event, "notifyListener:atValue:block:", {with_queue, 1, on_queue}));
    const auto again = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (queued->calls < 2 && std::chrono::steady_clock::now() < again) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(queued->calls == 2 && queued->value == 1 && queued->specific == marker, "a reached value notifies on the queue");
}

// -newBufferWithBytesNoCopy:length:options:deallocator: over page-aligned
// guest memory: contents is the client's pointer, the GPU sees its bytes, the
// deallocator runs (pointer, length) after release; a misaligned pointer
// answers nil without calling the deallocator.
void host_buffers(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  skipped\n"; return; }
    constexpr std::uint64_t size = 32768;
    const auto client = f.process.allocate_guest(size, 16384);
    for (std::uint64_t index = 0; index < 64; ++index) f.memory().write8(client + index, static_cast<std::uint8_t>(index + 1));
    struct Seen { std::atomic<int> calls{}; std::atomic<std::uint64_t> pointer{}, length{}; };
    auto seen = std::make_shared<Seen>();
    const auto deallocator = global_block(f, "metal_test_nocopy_deallocator", [seen](transmute::CpuState& s, transmute::GuestMemory&) {
        seen->pointer = s.x[1];
        seen->length = s.x[2];
        ++seen->calls;
    });
    const auto buffer = f.send(d, "newBufferWithBytesNoCopy:length:options:deallocator:", {client, size, 0, deallocator});
    if (!buffer) {
        // Only a device without VK_EXT_external_memory_host answers nil here.
        std::cout << "  no host-pointer import on this device: nil, as Metal fails\n";
        require(seen->calls == 0, "no deallocator call for a failed creation");
        return;
    }
    require(f.send(buffer, "contents") == client && f.send(buffer, "length") == size, "contents is the client's pointer");
    const auto copy = f.send(d, "newBufferWithLength:options:", {64, 0});
    const auto queue = f.send(d, "newCommandQueue");
    const auto commands = f.send(queue, "commandBuffer");
    const auto blit = f.send(commands, "blitCommandEncoder");
    alchemy::CallRegs copy_regs{};
    copy_regs.x[2] = buffer; copy_regs.x[3] = 0; copy_regs.x[4] = copy; copy_regs.x[5] = 0; copy_regs.x[6] = 64;
    static_cast<void>(f.objc.send(blit, "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:", copy_regs));
    static_cast<void>(f.send(blit, "endEncoding"));
    static_cast<void>(f.send(commands, "commit"));
    static_cast<void>(f.send(commands, "waitUntilCompleted"));
    const auto copied = f.send(copy, "contents");
    bool same = true;
    for (std::uint64_t index = 0; index < 64; ++index) same &= f.memory().read8(copied + index) == static_cast<std::uint8_t>(index + 1);
    require(same, "the GPU reads the client's bytes");
    static_cast<void>(f.send(buffer, "release"));
    // Deferred Vulkan destruction runs after later work completes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (seen->calls == 0 && std::chrono::steady_clock::now() < deadline) {
        const auto later = f.send(queue, "commandBuffer");
        static_cast<void>(f.send(later, "commit"));
        static_cast<void>(f.send(later, "waitUntilCompleted"));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(seen->calls == 1 && seen->pointer == client && seen->length == size, "the deallocator runs once with (pointer, length)");
    const auto misaligned = f.send(d, "newBufferWithBytesNoCopy:length:options:deallocator:", {client + 8, 4096, 0, deallocator});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(misaligned == 0 && seen->calls == 1, "a misaligned pointer: nil, no deallocator call");
}

void queues_and_logs(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  skipped\n"; return; }
    const auto descriptor = make(f, "MTLCommandQueueDescriptor");
    const auto queue = f.send(d, "newCommandQueueWithDescriptor:", {descriptor});
    require(queue != 0 && f.send(queue, "device") == d, "newCommandQueueWithDescriptor:");
    const auto buffer = f.send(queue, "commandBuffer");
    require(buffer != 0, "a queue from a descriptor makes command buffers");
    static_cast<void>(f.send(buffer, "commit"));
    static_cast<void>(f.send(buffer, "waitUntilCompleted"));
    require(f.send(buffer, "status") == 4, "MTLCommandBufferStatusCompleted");
    require(f.send(d, "newCommandQueueWithDescriptor:", {0}) == 0, "nil descriptor");
    const auto log_descriptor = make(f, "MTLLogStateDescriptor");
    const auto error = f.block(8);
    const auto log = f.send(d, "newLogStateWithDescriptor:error:", {log_descriptor, error});
    require(log != 0 && f.engine.read64(error) == 0, "newLogStateWithDescriptor:error:");
    require(f.send(log, "device") == d, "log state device");
    static_cast<void>(f.send(log, "addLogHandler:", {0}));
    static_cast<void>(f.send(log_descriptor, "setBufferSize:", {static_cast<Address>(-1)}));
    require(f.send(d, "newLogStateWithDescriptor:error:", {log_descriptor, error}) == 0, "negative buffer size fails");
    const auto failure = f.engine.read64(error);
    require(failure && f.send(failure, "code") == 1 && f.cf.utf8(f.send(failure, "domain")) == "MTLLogStateErrorDomain",
            "MTLLogStateErrorInvalidSize");
}

void shared_events(Fixture& f) {
    const auto d = device(f);
    if (!d) { std::cout << "  skipped\n"; return; }
    const auto event = f.send(d, "newSharedEvent");
    require(event && f.send(event, "signaledValue") == 0, "a new shared event starts at 0");
    static_cast<void>(f.send(event, "setSignaledValue:", {5}));
    require(f.send(event, "signaledValue") == 5, "CPU signal");
    require(f.send(event, "waitUntilSignaledValue:timeoutMS:", {5, 0}) == 1, "a reached value is waited at once");
    require(f.send(event, "waitUntilSignaledValue:timeoutMS:", {6, 20}) == 0, "an unreached value times out");
    // GPU-side: a command buffer that waits for 9 and then signals 10 does
    // not complete until the CPU signals 9.
    const auto queue = f.send(d, "newCommandQueue");
    const auto first = f.send(queue, "commandBuffer");
    static_cast<void>(f.send(first, "encodeWaitForEvent:value:", {event, 9}));
    static_cast<void>(f.send(first, "encodeSignalEvent:value:", {event, 10}));
    static_cast<void>(f.send(first, "commit"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(f.send(event, "signaledValue") == 5 && f.send(first, "status") != 4, "the GPU waits for the event");
    static_cast<void>(f.send(event, "setSignaledValue:", {9}));
    static_cast<void>(f.send(first, "waitUntilCompleted"));
    require(f.send(first, "status") == 4 && f.send(event, "signaledValue") == 10, "the GPU signals after the CPU's signal");
    const auto second = f.send(queue, "commandBuffer");
    static_cast<void>(f.send(second, "encodeSignalEvent:value:", {event, 11}));
    static_cast<void>(f.send(second, "commit"));
    static_cast<void>(f.send(second, "waitUntilCompleted"));
    require(f.send(event, "signaledValue") == 11, "a later command buffer signals");
    // MTLEvent (not shareable) orders command buffers the same way.
    const auto plain = f.send(d, "newEvent");
    require(plain && f.send(plain, "device") == d, "newEvent");
    const auto signal = f.send(queue, "commandBuffer");
    static_cast<void>(f.send(signal, "encodeSignalEvent:value:", {plain, 1}));
    const auto wait = f.send(queue, "commandBuffer");
    static_cast<void>(f.send(wait, "encodeWaitForEvent:value:", {plain, 1}));
    static_cast<void>(f.send(signal, "commit"));
    static_cast<void>(f.send(wait, "commit"));
    static_cast<void>(f.send(wait, "waitUntilCompleted"));
    require(f.send(wait, "status") == 4, "MTLEvent wait after its signal");
    // MTLFence: update in one encoder, wait in the next.
    const auto fence = f.send(d, "newFence");
    require(fence && f.send(fence, "device") == d, "newFence");
    const auto fenced = f.send(queue, "commandBuffer");
    const auto blit = f.send(fenced, "blitCommandEncoder");
    static_cast<void>(f.send(blit, "updateFence:", {fence}));
    static_cast<void>(f.send(blit, "endEncoding"));
    const auto compute = f.send(fenced, "computeCommandEncoder");
    static_cast<void>(f.send(compute, "waitForFence:", {fence}));
    static_cast<void>(f.send(compute, "endEncoding"));
    static_cast<void>(f.send(fenced, "commit"));
    static_cast<void>(f.send(fenced, "waitUntilCompleted"));
    require(f.send(fenced, "status") == 4, "fenced encoders complete");
}

} // namespace

int main() {
    return run("metal objects", {{"descriptors", descriptors}, {"counter names", counter_names}, {"identity", identity}, {"counters", counters},
                                 {"queues and logs", queues_and_logs}, {"shared events", shared_events},
                                 {"sampler lod clamps", sampler_lod_clamps}, {"shared event listeners", shared_event_listeners},
                                 {"host buffers", host_buffers}});
}
