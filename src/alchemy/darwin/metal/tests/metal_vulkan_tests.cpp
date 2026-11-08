// Metal-on-Vulkan headless tests (owner: A5): the host layer below the
// Objective-C providers, on the machine's Vulkan 1.3 GPU. Shaders are GLSL
// written to the MSL contract's fixed resource model and compiled to SPIR-V
// at build time; their Reflection is written by hand exactly as the MSL
// compiler reports it. Exit code 77 (ctest SKIP) when no device exists.
#include "alchemy/darwin/metal/bindless.hpp"
#include "alchemy/darwin/metal/commands.hpp"
#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/formats.hpp"
#include "alchemy/darwin/metal/pipeline.hpp"
#include "alchemy/darwin/metal/resources.hpp"
#include "alchemy/msl/compiler.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <new>
#include <thread>
#include <vector>

#ifdef ALCHEMY_METAL_TEST_SHADERS
#include "scale_comp.h"
#include "sample_frag.h"
#include "triangle_frag.h"
#include "triangle_vert.h"
#include "fetch_write_frag.h"
#include "fetch_read_frag.h"
#include "argument_samplers_frag.h"
#endif

namespace metal = alchemy::metal;
namespace msl = alchemy::msl;

namespace {

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

std::shared_ptr<metal::Buffer> shared_buffer(const std::shared_ptr<metal::Device>& device, std::size_t length) {
    return metal::Buffer::create(device, length, {metal::StorageMode::shared, 0});
}

// ---- profile -------------------------------------------------------------------------

void test_profile(const std::shared_ptr<metal::Device>& device) {
    const auto& p = device->profile();
    std::cout << "device: " << p.name << " (Vulkan " << VK_API_VERSION_MAJOR(device->api_version) << '.'
              << VK_API_VERSION_MINOR(device->api_version) << ")\n"
              << "  unified memory " << p.unified_memory << ", working set " << (p.recommended_working_set >> 20)
              << " MiB, max buffer " << (p.max_buffer_length >> 20) << " MiB\n"
              << "  threadgroup " << p.max_threads_per_threadgroup[0] << 'x' << p.max_threads_per_threadgroup[1] << 'x'
              << p.max_threads_per_threadgroup[2] << " (" << p.max_total_threads_per_threadgroup << " total, "
              << p.max_threadgroup_memory << " bytes), SIMD width " << p.thread_execution_width << '\n'
              << "  argument buffers tier " << (p.argument_buffers_tier + 1) << ", barycentrics " << p.barycentric_coordinates
              << ", BC " << p.bc_compression << ", float32 filtering " << p.float32_filtering << '\n'
              << "  families:";
    for (const auto family : p.families) std::cout << ' ' << family;
    std::cout << '\n';
    expect(p.supports_family(metal::gpu_family::common1), "Common1 family is claimed");
    expect(!p.supports_family(metal::gpu_family::apple7), "Apple7 is never claimed");
    expect(p.max_buffer_length >= (256ULL << 20), "maxBufferLength is at least 256 MiB");
    expect(metal::pixel_format_info(metal::pixel_format::bgra8_unorm)->vulkan == VK_FORMAT_B8G8R8A8_UNORM,
           "BGRA8Unorm maps to B8G8R8A8_UNORM");
    expect(metal::pixel_format_info(metal::pixel_format::rgb10a2_unorm)->vulkan == VK_FORMAT_A2B10G10R10_UNORM_PACK32,
           "RGB10A2Unorm maps to A2B10G10R10");
    expect(metal::srgb_partner(metal::pixel_format::rgba8_unorm) == metal::pixel_format::rgba8_unorm_srgb, "sRGB partner");
    expect(device->texture_format(metal::pixel_format::depth32_float_stencil8) == VK_FORMAT_D32_SFLOAT_S8_UINT,
           "Depth32Float_Stencil8 maps to D32_SFLOAT_S8_UINT");
}

// ---- buffers and blits ------------------------------------------------------------------

void test_buffers(const std::shared_ptr<metal::Device>& device) {
    constexpr std::size_t size = 4096;
    auto source = shared_buffer(device, size);
    auto destination = shared_buffer(device, size);
    auto staging_private = metal::Buffer::create(device, size, {metal::StorageMode::private_, 0});
    expect(source->contents() != nullptr, "shared buffer has contents");
    expect(staging_private->contents() == nullptr, "private buffer has no contents");
    expect(source->address != 0 && source->address % 256 == 0, "buffer device address is 256-byte aligned");
    for (std::size_t index = 0; index < size; ++index) source->contents()[index] = static_cast<std::uint8_t>(index * 7);
    std::memset(destination->contents(), 0, size);

    auto commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    commands->copy_buffer(source, 0, staging_private, 0, size);
    commands->copy_buffer(staging_private, 16, destination, 32, 1000);
    commands->fill_buffer(destination, 2001, 1003, 0x5a);   // unaligned head and tail
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "blit command buffer completes");
    expect(commands->status() == metal::CommandBufferStatus::completed, "blit status is completed");
    bool copied = true;
    for (std::size_t index = 0; index < 1000; ++index)
        copied &= destination->contents()[32 + index] == static_cast<std::uint8_t>((index + 16) * 7);
    expect(copied, "buffer -> private -> buffer copy round-trips");
    expect(destination->contents()[2000] == 0 && destination->contents()[3004] == 0, "fillBuffer leaves neighbours alone");
    bool filled = true;
    for (std::size_t index = 2001; index < 3004; ++index) filled &= destination->contents()[index] == 0x5a;
    expect(filled, "fillBuffer fills an unaligned range");

    // Private writes through staging.
    const std::vector<std::uint8_t> bytes(100, 0x33);
    staging_private->write(50, bytes);
    std::vector<std::uint8_t> back(100);
    staging_private->read(50, back);
    expect(back == bytes, "private buffer write/read through staging");
}

// newBufferWithBytesNoCopy: client memory imported as the buffer: the GPU
// reads what the CPU wrote through the client's pointer and the CPU sees GPU
// writes there; `released` runs once, after the GPU is done.
void test_host_buffers(const std::shared_ptr<metal::Device>& device) {
    if (!device->has_device_extension(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        std::cout << "SKIP: host buffers (no VK_EXT_external_memory_host)\n";
        std::string why;
        auto* bytes = static_cast<std::uint8_t*>(::operator new(65536, std::align_val_t{65536}));
        expect(!metal::Buffer::import_host(device, bytes, 65536, {metal::StorageMode::shared, 0}, {}, why) && !why.empty(),
               "without the extension the import is refused with a reason");
        ::operator delete(bytes, std::align_val_t{65536});
        return;
    }
    constexpr std::size_t size = 3 * 16384;
    auto* client = static_cast<std::uint8_t*>(::operator new(size, std::align_val_t{16384}));
    for (std::size_t index = 0; index < size; ++index) client[index] = static_cast<std::uint8_t>(index * 13);
    auto released = std::make_shared<std::atomic<int>>(0);
    std::string why;
    auto imported = metal::Buffer::import_host(device, client, size - 100, {metal::StorageMode::shared, 0},
                                               [released] { ++*released; }, why);
    expect(imported != nullptr, "host memory imported: " + why);
    expect(imported->contents() == client && imported->length == size - 100 && imported->address != 0,
           "contents is the client's pointer; length as asked; a device address");
    auto copy = shared_buffer(device, 4096);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    commands->copy_buffer(imported, 20000, copy, 0, 4096);   // GPU reads the client's bytes
    commands->fill_buffer(imported, 100, 50, 0xa5);          // GPU writes them
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "host-buffer blit completes");
    bool read = true;
    for (std::size_t index = 0; index < 4096; ++index) read &= copy->contents()[index] == static_cast<std::uint8_t>((20000 + index) * 13);
    expect(read, "the GPU reads what the CPU wrote through the pointer");
    bool written = client[99] == static_cast<std::uint8_t>(99 * 13) && client[150] == static_cast<std::uint8_t>(150 * 13);
    for (std::size_t index = 100; index < 150; ++index) written &= client[index] == 0xa5;
    expect(written, "the CPU sees the GPU's writes at the pointer");
    commands.reset();
    imported.reset();
    device->collect();
    for (int attempt = 0; attempt < 200 && released->load() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        device->collect();
    }
    expect(released->load() == 1, "the release callback runs once after the buffer is gone");
    expect(!metal::Buffer::import_host(device, client + 1, 4096, {metal::StorageMode::shared, 0}, {}, why) && !why.empty(),
           "a misaligned pointer is refused");
    expect(!metal::Buffer::import_host(device, client, 4096, {metal::StorageMode::private_, 0}, {}, why), "private storage is refused");
    ::operator delete(client, std::align_val_t{16384});
}

// ---- textures -----------------------------------------------------------------------------

void test_textures(const std::shared_ptr<metal::Device>& device) {
    metal::TextureDescriptor descriptor;
    descriptor.pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.width = 64;
    descriptor.height = 32;
    descriptor.mipmap_levels = 6;
    descriptor.usage = metal::texture_usage::shader_read | metal::texture_usage::render_target;
    auto texture = metal::Texture::create(device, descriptor);
    std::vector<std::uint8_t> pixels(64 * 32 * 4);
    for (std::size_t index = 0; index < pixels.size(); ++index) pixels[index] = static_cast<std::uint8_t>(index ^ (index >> 8));
    texture->replace_region({0, 0, 0, 64, 32, 1}, 0, 0, pixels, 64 * 4, 0);
    std::vector<std::uint8_t> back(pixels.size());
    texture->get_bytes(back, 64 * 4, 0, {0, 0, 0, 64, 32, 1}, 0, 0);
    expect(back == pixels, "replaceRegion/getBytes round-trip");

    // A sub-region with a padded row pitch.
    std::vector<std::uint8_t> region(8 * 300, 0);
    texture->get_bytes(region, 300, 0, {4, 2, 0, 8, 8, 1}, 0, 0);
    bool matches = true;
    for (std::uint32_t y = 0; y < 8; ++y)
        if (std::memcmp(region.data() + y * 300, pixels.data() + ((y + 2) * 64 + 4) * 4, 32) != 0) {
            if (matches)
                std::cerr << "row " << y << ": got " << int(region[y * 300]) << " expected "
                          << int(pixels[((y + 2) * 64 + 4) * 4]) << '\n';
            matches = false;
        }
    expect(matches, "getBytes of a sub-region with a padded bytesPerRow");

    // Mipmaps from a solid level 0, then a blit readback of level 3.
    std::vector<std::uint8_t> solid(64 * 32 * 4);
    for (std::size_t index = 0; index < solid.size(); index += 4) {
        solid[index] = 200; solid[index + 1] = 100; solid[index + 2] = 50; solid[index + 3] = 255;
    }
    texture->replace_region({0, 0, 0, 64, 32, 1}, 0, 0, solid, 64 * 4, 0);
    auto readback = shared_buffer(device, 8 * 4 * 4);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    commands->generate_mipmaps(texture);
    commands->copy_texture_to_buffer(texture, 0, 3, {0, 0, 0, 8, 4, 1}, readback, 0, 8 * 4, 0);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "mipmap command buffer completes");
    const auto* level3 = readback->contents();
    expect(level3[0] == 200 && level3[1] == 100 && level3[2] == 50 && level3[3] == 255, "generateMipmaps fills level 3");

    // A view of level 1 with a swizzle has level 1's size.
    auto view = metal::Texture::create_view(texture, metal::pixel_format::rgba8_unorm_srgb, metal::TextureType::type2d, 1, 2, 0, 1,
                                            {4, 3, 2, 5});
    expect(view->descriptor.width == 32 && view->descriptor.height == 16 && view->view, "texture view of level 1");

    metal::SamplerDescriptor sampler;
    sampler.min_filter = sampler.mag_filter = 1;
    sampler.mip_filter = 2;
    sampler.address = {2, 0, 5};
    sampler.max_anisotropy = 8;
    expect(metal::Sampler::create(device, sampler)->sampler != VK_NULL_HANDLE, "sampler state");
}

// Depth/stencil textures created without MTLTextureUsageRenderTarget are
// still attachable (Blender's depth upload draws into such a texture); a
// color texture without it fails explicitly instead of reaching the driver.
void test_attachment_usage(const std::shared_ptr<metal::Device>& device) {
    metal::TextureDescriptor depth_descriptor;
    depth_descriptor.pixel_format = metal::pixel_format::depth32_float_stencil8;
    depth_descriptor.width = depth_descriptor.height = 16;
    depth_descriptor.usage = metal::texture_usage::shader_read;
    depth_descriptor.options = {metal::StorageMode::private_};
    auto depth = metal::Texture::create(device, depth_descriptor);
    expect((depth->image_usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0,
           "a ShaderRead-only depth/stencil texture is created attachable");
    metal::RenderPass pass;
    pass.depth.texture = depth;
    pass.depth.load_action = 2;   // clear
    pass.depth.store_action = 1;  // store
    pass.depth.clear_depth = 1.0;
    pass.stencil = pass.depth;
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "depth-only pass on a ShaderRead-only depth texture completes");

    // Color textures are likewise attachable and writable whatever their
    // Metal usage, when the format allows it (Metal treats the usage as a
    // hint; Blender binds ShaderRead-only textures to read_write arguments).
    metal::TextureDescriptor color_descriptor;
    color_descriptor.width = color_descriptor.height = 16;
    color_descriptor.usage = metal::texture_usage::shader_read;
    auto color = metal::Texture::create(device, color_descriptor);
    expect((color->image_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0,
           "a ShaderRead-only color texture is created attachable");
    expect(color->attachment_view(0, 0, 1) != VK_NULL_HANDLE, "a ShaderRead-only color texture is an attachment");
    expect(color->storage_view != VK_NULL_HANDLE && color->storage_view == color->view,
           "a ShaderRead-only RGBA8 texture has one sampled+storage view");
    // An sRGB view of the (storage-capable, mutable) RGBA8Unorm image: sampled
    // and attachable, never a storage view (sRGB formats are not storage).
    auto srgb = metal::Texture::create_view(color, metal::pixel_format::rgba8_unorm_srgb, metal::TextureType::type2d, 0, 1, 0, 1,
                                            {2, 3, 4, 5});
    expect(srgb->view && !srgb->storage_view, "an sRGB view has a sampled view and no storage view");
    expect(srgb->attachment_view(0, 0, 1) != VK_NULL_HANDLE, "an sRGB view of a storage-capable image is attachable");
    // A swizzled view samples through the swizzle and stores through an
    // identity view.
    auto swizzled = metal::Texture::create_view(color, metal::pixel_format::rgba8_unorm, metal::TextureType::type2d, 0, 1, 0, 1,
                                                {4, 3, 2, 5});
    expect(swizzled->view && swizzled->storage_view && swizzled->view != swizzled->storage_view,
           "a swizzled view has separate sampled and storage views");
    // Another format without PixelFormatView is refused explicitly.
    metal::TextureDescriptor half_descriptor;
    half_descriptor.pixel_format = metal::pixel_format::rgba16_float;
    half_descriptor.width = half_descriptor.height = 16;
    auto half = metal::Texture::create(device, half_descriptor);
    bool refused = false;
    try {
        static_cast<void>(metal::Texture::create_view(half, metal::pixel_format::rg32_float, metal::TextureType::type2d,
                                                      0, 1, 0, 1, {2, 3, 4, 5}));
    } catch (const std::exception& error) {
        refused = std::string(error.what()).find("MTLTextureUsagePixelFormatView") != std::string::npos;
    }
    expect(refused, "a view of another format needs MTLTextureUsagePixelFormatView");
    if (device->profile().bc_compression) {
        metal::TextureDescriptor compressed_descriptor;
        compressed_descriptor.pixel_format = metal::pixel_format::bc1_rgba;
        compressed_descriptor.width = compressed_descriptor.height = 16;
        auto compressed = metal::Texture::create(device, compressed_descriptor);
        refused = false;
        try {
            static_cast<void>(compressed->attachment_view(0, 0, 1));
        } catch (const std::exception& error) {
            refused = std::string(error.what()).find("cannot be one") != std::string::npos;
        }
        expect(refused, "a compressed texture is refused as an attachment");
    }
}

// Integer vertex formats follow the shader input's signedness (Blender's
// MTLVertexFormatInt2 glyph_size attribute read as a uint2 SPIR-V input).
void test_vertex_signedness() {
    expect(metal::vertex_format_with_signedness(VK_FORMAT_R32G32_SINT, false) == VK_FORMAT_R32G32_UINT,
           "Int2 into an unsigned input is R32G32_UINT");
    expect(metal::vertex_format_with_signedness(VK_FORMAT_R8G8B8A8_UINT, true) == VK_FORMAT_R8G8B8A8_SINT,
           "UChar4 into a signed input is R8G8B8A8_SINT");
    expect(metal::vertex_format_with_signedness(VK_FORMAT_R32G32_SINT, true) == VK_FORMAT_R32G32_SINT,
           "matching signedness is kept");
    expect(metal::vertex_format_with_signedness(VK_FORMAT_R8G8B8A8_UNORM, false) == VK_FORMAT_R8G8B8A8_UNORM,
           "normalized formats are kept");
}

// ---- events ----------------------------------------------------------------------------------

void test_events(const std::shared_ptr<metal::Device>& device) {
    auto event = metal::Event::create(device, true);
    auto buffer = shared_buffer(device, 256);
    std::memset(buffer->contents(), 0, 256);
    auto first = metal::CommandBuffer::create(device);
    first->begin_blit();
    first->fill_buffer(buffer, 0, 128, 1);
    first->end_encoding();
    first->encode_signal(event, 3);
    first->commit();
    auto second = metal::CommandBuffer::create(device);
    second->encode_wait(event, 3);
    second->begin_blit();
    second->fill_buffer(buffer, 128, 128, 2);
    second->end_encoding();
    second->commit();
    expect(second->wait(UINT64_MAX), "event-waiting command buffer completes");
    expect(event->signaled_value() == 3, "encodeSignalEvent: sets the shared event's value");
    expect(buffer->contents()[0] == 1 && buffer->contents()[255] == 2, "both batches ran");

    // A CPU signal releases a waiting command buffer.
    auto third = metal::CommandBuffer::create(device);
    third->encode_wait(event, 5);
    third->begin_blit();
    third->fill_buffer(buffer, 0, 4, 9);
    third->end_encoding();
    third->commit();
    expect(!third->wait(20'000'000), "the command buffer waits for the event");
    event->signal(5);
    expect(third->wait(UINT64_MAX) && buffer->contents()[0] == 9, "setSignaledValue: releases the wait");
}

// ---- timestamp counters ---------------------------------------------------------------------------

// Encoder start/end samples are GPU timestamps in submission order;
// resolveCounters: copies them, MTLCounterErrorValue for unwritten samples;
// the CPU resolve agrees; a resampled index is rewritten.
void test_timestamps(const std::shared_ptr<metal::Device>& device) {
    if (!device->profile().timestamps) { std::cout << "SKIP: timestamps (no graphics+compute timestamps)\n"; return; }
    auto samples = metal::CounterSampleBuffer::create(device, 4);
    expect(samples->resolve(0, 4) == std::vector<std::uint64_t>(4, metal::CounterSampleBuffer::error_value), "new samples are unwritten");
    auto buffer = shared_buffer(device, 64);
    std::memset(buffer->contents(), 0, 64);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_compute(false, {{{samples, 0}}, {{samples, 1}}});
    commands->end_encoding();
    commands->begin_blit({{{samples, 2}}, {}});
    commands->fill_buffer(buffer, 32, 32, 7);
    commands->resolve_counters(samples, 0, 4, buffer, 0);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "the sampled command buffer completes");
    const auto cpu = samples->resolve(0, 4);
    std::uint64_t gpu[4]{};
    std::memcpy(gpu, buffer->contents(), sizeof(gpu));
    expect(cpu[0] != metal::CounterSampleBuffer::error_value && cpu[1] != metal::CounterSampleBuffer::error_value &&
           cpu[2] != metal::CounterSampleBuffer::error_value, "written samples resolve");
    expect(cpu[0] <= cpu[1] && cpu[1] <= cpu[2], "samples follow submission order");
    expect(cpu[3] == metal::CounterSampleBuffer::error_value, "an unwritten sample is MTLCounterErrorValue");
    expect(gpu[0] == cpu[0] && gpu[1] == cpu[1] && gpu[2] == cpu[2], "resolveCounters: copies the samples");
    expect(gpu[3] == metal::CounterSampleBuffer::error_value, "resolveCounters: leaves MTLCounterErrorValue for unwritten samples");
    expect(buffer->contents()[32] == 7, "the blit's own work ran");
    auto again = metal::CommandBuffer::create(device);
    again->begin_blit({{{samples, 0}}, {}});
    again->end_encoding();
    again->commit();
    expect(again->wait(UINT64_MAX) && samples->resolve(0, 1)[0] >= cpu[2], "a resampled index is rewritten later");
    if (const auto now = device->gpu_timestamp()) expect(*now >= cpu[2], "the calibrated GPU clock is past the samples");
}

// ---- concurrent completion ---------------------------------------------------------------------

// waitUntilCompleted and status from several threads while another thread
// (the run loop's completion thread in the providers) finalizes: once wait()
// returns, every thread sees "completed" - never "scheduled" while a
// concurrent finalizer is still releasing resources - and the completion
// work runs exactly once.
void test_concurrent_completion(const std::shared_ptr<metal::Device>& device) {
    constexpr int rounds = 12, waiters = 3, pollers = 3;
    int stale = 0, early = 0, repeated = 0;
    for (int round = 0; round < rounds; ++round) {
        auto event = metal::Event::create(device, true);
        auto buffer = shared_buffer(device, 256);
        auto commands = metal::CommandBuffer::create(device);
        std::atomic<int> completions{0};
        // A slow completion step widens the window in which one thread is
        // finalizing while the others observe the timeline already passed.
        commands->on_complete([&completions] {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            completions.fetch_add(1);
        });
        commands->encode_wait(event, 1);   // held on the GPU until the CPU signal
        commands->begin_blit();
        commands->fill_buffer(buffer, 0, 256, 7);
        commands->end_encoding();
        commands->commit();

        std::atomic<bool> go{false};
        std::atomic<int> stale_seen{0}, early_seen{0};
        std::vector<std::thread> threads;
        for (int index = 0; index < waiters; ++index)
            threads.emplace_back([&] {
                while (!go.load()) std::this_thread::yield();
                while (!commands->wait(1'000'000)) {}
                if (commands->status() != metal::CommandBufferStatus::completed) stale_seen.fetch_add(1);
                if (completions.load() != 1) early_seen.fetch_add(1);
            });
        for (int index = 0; index < pollers; ++index)
            threads.emplace_back([&] {
                while (!go.load()) std::this_thread::yield();
                auto status = commands->status();
                while (status != metal::CommandBufferStatus::completed && status != metal::CommandBufferStatus::error)
                    status = commands->status();
                if (status != metal::CommandBufferStatus::completed) stale_seen.fetch_add(1);
                if (completions.load() != 1) early_seen.fetch_add(1);
            });
        // The completion thread's role: Device::collect runs the deferred
        // finalize step once the timeline passes.
        threads.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            while (device->completed_value() < commands->timeline_value()) std::this_thread::yield();
            device->collect();
        });
        go = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        event->signal(1);
        for (auto& thread : threads) thread.join();
        stale += stale_seen.load();
        early += early_seen.load();
        if (completions.load() != 1) ++repeated;
        expect(buffer->contents()[255] == 7, "the held command buffer ran");
    }
    expect(stale == 0, "status() is completed once waitUntilCompleted returns or completion is observed (" +
                           std::to_string(stale) + " stale)");
    expect(early == 0, "completion work finished before any thread observes completion (" + std::to_string(early) + ")");
    expect(repeated == 0, "completion work runs exactly once per command buffer");
}

// ---- wrapped images with a shared layout (QuartzCore drawables) -------------------------------

void test_wrapped_layout(const std::shared_ptr<metal::Device>& device) {
    // The image's owner (QuartzCore) and Metal track one layout: Metal
    // transitions from whatever the owner left (keeping the contents) and
    // publishes GENERAL for the owner's next use.
    metal::TextureDescriptor descriptor;
    descriptor.pixel_format = metal::pixel_format::bgra8_unorm;
    descriptor.width = 16;
    descriptor.height = 8;
    descriptor.usage = metal::texture_usage::shader_read | metal::texture_usage::render_target;
    auto owner = metal::Texture::create(device, descriptor);
    std::vector<std::uint8_t> pixels(16 * 8 * 4);
    for (std::size_t index = 0; index < pixels.size(); ++index) pixels[index] = static_cast<std::uint8_t>(index * 3 + 1);
    owner->replace_region({0, 0, 0, 16, 8, 1}, 0, 0, pixels, 16 * 4, 0);

    // The owner moves the image to TRANSFER_SRC_OPTIMAL (its present blit).
    device->immediate([&](VkCommandBuffer command) {
        VkImageMemoryBarrier2 value{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        value.srcStageMask = value.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        value.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        value.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        value.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        value.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        value.srcQueueFamilyIndex = value.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        value.image = owner->image;
        value.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &value;
        device->vk.vkCmdPipelineBarrier2(command, &dependency);
    });
    auto layout = std::make_shared<std::atomic<VkImageLayout>>(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    auto wrapped = metal::Texture::wrap(device, owner->image, descriptor, VK_IMAGE_LAYOUT_UNDEFINED, layout);
    expect(wrapped->state->layout == layout && wrapped->state->external, "wrap shares the owner's layout");

    auto readback = shared_buffer(device, pixels.size());
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    commands->copy_texture_to_buffer(wrapped, 0, 0, {0, 0, 0, 16, 8, 1}, readback, 0, 16 * 4, 0);
    commands->end_encoding();
    expect(layout->load() == VK_IMAGE_LAYOUT_GENERAL, "Metal publishes GENERAL to the owner");
    commands->commit();
    expect(commands->wait(UINT64_MAX), "wrapped-image command buffer completes");
    expect(std::memcmp(readback->contents(), pixels.data(), pixels.size()) == 0,
           "the transition from the owner's layout keeps the contents");
    wrapped.reset();
    expect(owner->image != VK_NULL_HANDLE, "a wrapped texture never destroys the image");
}

#ifdef ALCHEMY_METAL_TEST_SHADERS
// glslang caps constant_id at 2047: renumber 1000-1002 to the contract's
// reserved workgroup-size ids and 1003 to function constant 0's "defined" id.
std::vector<std::uint32_t> words(const std::uint32_t* code, std::size_t bytes) {
    std::vector<std::uint32_t> result(code, code + bytes / 4);
    for (std::size_t index = 5; index < result.size();) {
        const auto count = result[index] >> 16, opcode = result[index] & 0xffff;
        if (!count) throw std::runtime_error("malformed SPIR-V");
        if (opcode == 71 && count == 4 && result[index + 2] == 1 && result[index + 3] >= 1000 && result[index + 3] <= 1002)
            result[index + 3] = msl::reserved_spec_id::workgroup_size_x + (result[index + 3] - 1000);
        else if (opcode == 71 && count == 4 && result[index + 2] == 1 && result[index + 3] == 1003)
            result[index + 3] = msl::reserved_spec_id::function_constant_defined_base;
        index += count;
    }
    return result;
}

msl::Type scalar(msl::Scalar kind, std::uint32_t size) {
    msl::Type type;
    type.kind = msl::Type::Kind::scalar;
    type.scalar = kind;
    type.size = type.alignment = size;
    return type;
}

void test_compute(const std::shared_ptr<metal::Device>& device) {
    msl::Function function;
    function.name = "main";
    function.stage = msl::Stage::kernel;
    function.spirv = words(scale_comp_spirv, sizeof(scale_comp_spirv));
    auto& r = function.reflection;
    r.stage = msl::Stage::kernel;
    r.types.push_back(scalar(msl::Scalar::uint32, 4));
    r.argument_table = {0, msl::argument_table_binding, msl::DescriptorType::uniform_buffer, 1};
    // Smaller than the slots the kernel reads, as for a [[buffer(fc + n)]]
    // binding whose slot a function constant picks: the table covers it.
    r.argument_table_size = 8;
    r.buffers.push_back({0, "source", msl::BufferBinding::Space::device, msl::Access::read, 0});
    r.buffers.push_back({1, "destination", msl::BufferBinding::Space::device, msl::Access::write, 0});
    r.function_constants.push_back({0, "multiplier", 0, true});
    r.builtins = {msl::Builtin::thread_position_in_grid, msl::Builtin::threads_per_grid};
    metal::ShaderLibrary library(device, {function});
    auto shader = std::make_shared<metal::ShaderFunction>(device, library.find("main"));
    metal::ConstantValues missing;
    expect(shader->specialize(missing).has_value(), "a required function constant without a value is an error");
    metal::ConstantValues values;
    const std::uint32_t three = 3;
    values.by_name["multiplier"] = {33, {reinterpret_cast<const std::uint8_t*>(&three), reinterpret_cast<const std::uint8_t*>(&three) + 4}};
    expect(!shader->specialize(values).has_value(), "function constants by name");
    auto pipeline = metal::ComputePipeline::create(device, shader);
    expect(pipeline->max_total_threads >= 64, "maxTotalThreadsPerThreadgroup >= 64");
    expect(pipeline->reads_grid, "the kernel's push-constant grid block is detected");

    constexpr std::uint32_t count = 1000, capacity = 1024;
    auto source = shared_buffer(device, capacity * 4);
    auto destination = shared_buffer(device, capacity * 4);
    auto* input = reinterpret_cast<std::uint32_t*>(source->contents());
    for (std::uint32_t index = 0; index < capacity; ++index) input[index] = index + 1;
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    commands->fill_buffer(destination, 0, capacity * 4, 0xab);
    commands->end_encoding();
    commands->begin_compute(false);
    commands->set_compute_pipeline(pipeline);
    commands->set_buffer(metal::Stage::kernel, 0, source, 0);
    commands->set_buffer(metal::Stage::kernel, 1, destination, 0);
    commands->dispatch_threads({count, 1, 1}, {64, 1, 1});   // 1000 is not a multiple of 64
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "compute command buffer completes");
    const auto* output = reinterpret_cast<const std::uint32_t*>(destination->contents());
    bool correct = true;
    for (std::uint32_t index = 0; index < count; ++index) correct &= output[index] == (index + 1) * 3 + index;
    expect(correct, "kernel output (function constant 3 and defined, argument-table buffers)");
    bool untouched = true;
    for (std::uint32_t index = count; index < capacity; ++index) untouched &= output[index] == 0xabababab;
    expect(untouched, "dispatchThreads: masks threads outside the grid");

    // setBytes source and an offset destination, dispatchThreadgroups.
    std::vector<std::uint32_t> small(32);
    for (std::uint32_t index = 0; index < 32; ++index) small[index] = 100 + index;
    commands = metal::CommandBuffer::create(device);
    commands->begin_compute(false);
    commands->set_compute_pipeline(pipeline);
    commands->set_bytes(metal::Stage::kernel, 0, {reinterpret_cast<const std::uint8_t*>(small.data()), small.size() * 4});
    commands->set_buffer(metal::Stage::kernel, 1, destination, 0);
    commands->set_buffer_offset(metal::Stage::kernel, 1, 2048);
    commands->dispatch_threadgroups({2, 1, 1}, {16, 1, 1});
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "second compute command buffer completes");
    correct = true;
    for (std::uint32_t index = 0; index < 32; ++index) correct &= output[512 + index] == (100 + index) * 3 + index;
    expect(correct, "setBytes: and setBufferOffset:atIndex:");
}

msl::Function vertex_function() {
    msl::Function function;
    function.name = "main";
    function.stage = msl::Stage::vertex;
    function.spirv = std::vector<std::uint32_t>(triangle_vert_spirv, triangle_vert_spirv + sizeof(triangle_vert_spirv) / 4);
    auto& r = function.reflection;
    r.stage = msl::Stage::vertex;
    msl::Type float4;
    float4.kind = msl::Type::Kind::vector;
    float4.scalar = msl::Scalar::float32;
    float4.components = 4;
    float4.size = float4.alignment = 16;
    r.types.push_back(float4);
    r.argument_table = {0, 0, msl::DescriptorType::uniform_buffer, 1};
    r.argument_table_size = 16;
    r.buffers.push_back({0, "vertices", msl::BufferBinding::Space::device, msl::Access::read, 0});
    r.buffers.push_back({1, "colors", msl::BufferBinding::Space::constant, msl::Access::read, 0});
    r.stage_in.push_back({0, "position", 0});
    r.outputs.push_back({"color", 0, 0});
    r.builtins = {msl::Builtin::vertex_id, msl::Builtin::position};
    return function;
}

msl::Function fragment_function(bool sampled) {
    msl::Function function;
    function.name = "main";
    function.stage = msl::Stage::fragment;
    if (sampled) function.spirv.assign(sample_frag_spirv, sample_frag_spirv + sizeof(sample_frag_spirv) / 4);
    else function.spirv.assign(triangle_frag_spirv, triangle_frag_spirv + sizeof(triangle_frag_spirv) / 4);
    auto& r = function.reflection;
    r.stage = msl::Stage::fragment;
    msl::Type float4;
    float4.kind = msl::Type::Kind::vector;
    float4.scalar = msl::Scalar::float32;
    float4.components = 4;
    float4.size = float4.alignment = 16;
    r.types.push_back(float4);
    r.inputs.push_back({"color", 0, 0});
    r.color_outputs.push_back({0, 0, 0});
    if (sampled) {
        msl::TextureBinding texture;
        texture.index = 0;
        texture.name = "image";
        texture.descriptor = {1, msl::texture_binding_base, msl::DescriptorType::sampled_image, 1};
        r.textures.push_back(texture);
        msl::SamplerBinding sampler;
        sampler.index = 0;
        sampler.name = "point_sampler";
        sampler.descriptor = {1, msl::sampler_binding_base, msl::DescriptorType::sampler, 1};
        r.samplers.push_back(sampler);
    } else {
        r.argument_table = {1, 0, msl::DescriptorType::uniform_buffer, 1};
        r.argument_table_size = 8;
        r.buffers.push_back({0, "tint", msl::BufferBinding::Space::constant, msl::Access::read, 0});
    }
    return function;
}

std::array<std::uint8_t, 4> pixel(const std::uint8_t* image, std::uint32_t pitch, std::uint32_t x, std::uint32_t y) {
    const auto* p = image + y * pitch + x * 4;
    return {p[0], p[1], p[2], p[3]};
}

// Moves an image to `layout` from GENERAL, as QuartzCore's present blit does.
void owner_transition(const std::shared_ptr<metal::Device>& device, VkImage image, VkImageLayout layout) {
    device->immediate([&](VkCommandBuffer command) {
        VkImageMemoryBarrier2 value{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        value.srcStageMask = value.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        value.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        value.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        value.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        value.newLayout = layout;
        value.srcQueueFamilyIndex = value.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        value.image = image;
        value.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &value;
        device->vk.vkCmdPipelineBarrier2(command, &dependency);
    });
}

// `wrapped`: the sampled texture is an external image (a drawable) whose
// owner left it in TRANSFER_SRC_OPTIMAL; it is first bound inside the render
// pass, so the command buffer transitions it at its start.
void test_render(const std::shared_ptr<metal::Device>& device, bool sampled, bool wrapped = false) {
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(fragment_function(sampled)));
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};   // MTLVertexFormatFloat2, offset 0, buffer 0
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    descriptor.label = sampled ? "sampled" : "triangle";
    auto pipeline = metal::RenderPipeline::create(device, descriptor);

    metal::TextureDescriptor target_descriptor;
    target_descriptor.pixel_format = metal::pixel_format::rgba8_unorm;
    target_descriptor.width = target_descriptor.height = 64;
    target_descriptor.usage = metal::texture_usage::render_target | metal::texture_usage::shader_read;
    target_descriptor.options = {metal::StorageMode::private_};
    auto target = metal::Texture::create(device, target_descriptor);

    // Metal NDC (+Y up): a triangle over the top half, clockwise on screen
    // (Metal's default front face), drawn with back-face culling.
    const float positions[] = {-1, 1, 1, 1, 0, 0};
    auto vertices = shared_buffer(device, sizeof(positions));
    std::memcpy(vertices->contents(), positions, sizeof(positions));
    const float colors[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    const float tint[] = {1, 1, 0.5F, 1};

    std::shared_ptr<metal::Texture> image, owner;
    std::shared_ptr<std::atomic<VkImageLayout>> owner_layout;
    if (sampled) {
        metal::TextureDescriptor image_descriptor;
        image_descriptor.width = image_descriptor.height = 2;
        image = metal::Texture::create(device, image_descriptor);
        const std::uint8_t texels[] = {10, 20, 30, 255, 0, 255, 0, 255, 40, 50, 60, 255, 70, 80, 90, 255};
        image->replace_region({0, 0, 0, 2, 2, 1}, 0, 0, texels, 8, 0);
        if (wrapped) {
            owner = std::move(image);
            owner_transition(device, owner->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            owner_layout = std::make_shared<std::atomic<VkImageLayout>>(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            image = metal::Texture::wrap(device, owner->image, image_descriptor, VK_IMAGE_LAYOUT_UNDEFINED, owner_layout);
        }
    }
    metal::SamplerDescriptor point;   // nearest, clamp

    metal::RenderPass pass;
    pass.colors[0].texture = target;
    pass.colors[0].load_action = 2;    // clear
    pass.colors[0].store_action = 1;   // store
    pass.colors[0].clear_color = {0, 0, 1, 1};
    auto readback = shared_buffer(device, 64 * 256);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->set_render_pipeline(pipeline);
    commands->set_cull_mode(2);   // back
    commands->set_buffer(metal::Stage::vertex, 0, vertices, 0);
    commands->set_bytes(metal::Stage::vertex, 1, {reinterpret_cast<const std::uint8_t*>(colors), sizeof(colors)});
    if (sampled) {
        commands->set_texture(metal::Stage::fragment, 0, image);
        commands->set_sampler(metal::Stage::fragment, 0, metal::Sampler::create(device, point));
    } else {
        commands->set_bytes(metal::Stage::fragment, 0, {reinterpret_cast<const std::uint8_t*>(tint), sizeof(tint)});
    }
    commands->draw(3, 0, 3, 1, 0);   // MTLPrimitiveTypeTriangle
    commands->end_encoding();
    if (wrapped) expect(owner_layout->load() == VK_IMAGE_LAYOUT_GENERAL, "a drawable sampled in a render pass is published GENERAL");
    commands->begin_blit();
    commands->copy_texture_to_buffer(target, 0, 0, {0, 0, 0, 64, 64, 1}, readback, 0, 256, 64 * 256);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "render command buffer completes");
    const auto* bytes = readback->contents();
    const auto inside = pixel(bytes, 256, 32, 4), outside = pixel(bytes, 256, 32, 60), corner = pixel(bytes, 256, 1, 40);
    const std::array<std::uint8_t, 4> blue{0, 0, 255, 255};
    const std::array<std::uint8_t, 4> expected = sampled ? std::array<std::uint8_t, 4>{0, 255, 0, 255}
                                                         : std::array<std::uint8_t, 4>{255, 0, 0, 255};
    char line[160];
    std::snprintf(line, sizeof(line), "pixel (32,4) = %u %u %u %u, (32,60) = %u %u %u %u", inside[0], inside[1], inside[2],
                  inside[3], outside[0], outside[1], outside[2], outside[3]);
    const std::string kind = wrapped ? "wrapped-sampled " : sampled ? "sampled " : "";
    expect(inside == expected, kind + "triangle covers the top (Y up, clockwise front): " + line);
    expect(outside == blue && corner == blue, kind + "clear color outside the triangle: " + line);
}

// Occlusion queries through a visibility result buffer in `storage` (Dawn's is
// private): counting mode at offset 0 around the top-half triangle, at offset 8
// around no draw. A blit in the same command buffer reads the results, as
// Dawn's query resolve does, so they are in the buffer when the pass ends.
void test_visibility_results(const std::shared_ptr<metal::Device>& device, metal::StorageMode storage) {
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(fragment_function(false)));
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    descriptor.label = "visibility";
    auto pipeline = metal::RenderPipeline::create(device, descriptor);
    metal::TextureDescriptor target_descriptor;
    target_descriptor.pixel_format = metal::pixel_format::rgba8_unorm;
    target_descriptor.width = target_descriptor.height = 64;
    target_descriptor.usage = metal::texture_usage::render_target;
    target_descriptor.options = {metal::StorageMode::private_};

    const float positions[] = {-1, 1, 1, 1, 0, 0};
    auto vertices = shared_buffer(device, sizeof(positions));
    std::memcpy(vertices->contents(), positions, sizeof(positions));
    const float colors[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    const float tint[] = {1, 1, 1, 1};
    auto visibility = metal::Buffer::create(device, 16, {storage, 0});
    auto readback = shared_buffer(device, 16);
    std::memset(readback->contents(), 0xab, 16);

    metal::RenderPass pass;
    pass.colors[0].texture = metal::Texture::create(device, target_descriptor);
    pass.colors[0].load_action = 2;
    pass.colors[0].store_action = 1;
    pass.visibility = visibility;
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->set_render_pipeline(pipeline);
    commands->set_buffer(metal::Stage::vertex, 0, vertices, 0);
    commands->set_bytes(metal::Stage::vertex, 1, {reinterpret_cast<const std::uint8_t*>(colors), sizeof(colors)});
    commands->set_bytes(metal::Stage::fragment, 0, {reinterpret_cast<const std::uint8_t*>(tint), sizeof(tint)});
    commands->set_visibility_mode(2, 0);   // MTLVisibilityResultModeCounting
    commands->draw(3, 0, 3, 1, 0);
    commands->set_visibility_mode(2, 8);
    commands->set_visibility_mode(0, 8);   // disabled
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_buffer(visibility, 0, readback, 0, 16);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "visibility command buffer completes");
    std::uint64_t results[2]{};
    std::memcpy(results, readback->contents(), sizeof(results));
    const std::string kind = storage == metal::StorageMode::private_ ? "private" : "shared";
    expect(results[0] > 0 && results[0] != 0xababababababababULL,
           kind + " visibility buffer: the triangle's query passed samples (" + std::to_string(results[0]) + ")");
    expect(results[1] == 0, kind + " visibility buffer: the query around no draw is 0 (" + std::to_string(results[1]) + ")");
}

// A fragment function of `code` with a float4 color input and float4 color
// outputs 0..outputs-1, as the MSL compiler reflects it.
msl::Function plain_fragment(const std::uint32_t* code, std::size_t bytes, std::uint32_t outputs) {
    msl::Function function;
    function.name = "main";
    function.stage = msl::Stage::fragment;
    function.spirv.assign(code, code + bytes / 4);
    auto& r = function.reflection;
    r.stage = msl::Stage::fragment;
    msl::Type float4;
    float4.kind = msl::Type::Kind::vector;
    float4.scalar = msl::Scalar::float32;
    float4.components = 4;
    float4.size = float4.alignment = 16;
    r.types.push_back(float4);
    r.inputs.push_back({"color", 0, 0});
    for (std::uint32_t index = 0; index < outputs; ++index) r.color_outputs.push_back({index, 0, 0});
    return function;
}

// Draws the top-half triangle once per pipeline into `pass` (`setup` binds
// fragment resources) and returns color attachment 0 read back (64x64).
std::shared_ptr<metal::Buffer> draw_top_half(const std::shared_ptr<metal::Device>& device, const metal::RenderPass& pass,
    const std::vector<std::shared_ptr<metal::RenderPipeline>>& pipelines,
    const std::function<void(metal::CommandBuffer&, std::size_t)>& setup) {
    const float positions[] = {-1, 1, 1, 1, 0, 0};
    auto vertices = shared_buffer(device, sizeof(positions));
    std::memcpy(vertices->contents(), positions, sizeof(positions));
    const float colors[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    auto readback = shared_buffer(device, 64 * 256);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    for (std::size_t index = 0; index < pipelines.size(); ++index) {
        commands->set_render_pipeline(pipelines[index]);
        commands->set_buffer(metal::Stage::vertex, 0, vertices, 0);
        commands->set_bytes(metal::Stage::vertex, 1, {reinterpret_cast<const std::uint8_t*>(colors), sizeof(colors)});
        setup(*commands, index);
        commands->draw(3, 0, 3, 1, 0);
    }
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_texture_to_buffer(pass.colors[0].texture, 0, 0, {0, 0, 0, 64, 64, 1}, readback, 0, 256, 64 * 256);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "command buffer completes");
    return readback;
}

std::shared_ptr<metal::Texture> render_target(const std::shared_ptr<metal::Device>& device);
std::string format_pixel(const std::array<std::uint8_t, 4>& p);

// setTriangleFillMode:Lines and setDepthClipMode:Clamp (pipeline variants):
// the top-half triangle drawn as lines leaves its interior clear and lights
// its edges; drawn with depth clamp (z = 0, inside the range) it fills as usual.
void test_raster_variants(const std::shared_ptr<metal::Device>& device) {
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(fragment_function(false)));
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    descriptor.label = "raster variants";
    auto pipeline = metal::RenderPipeline::create(device, descriptor);
    metal::TextureDescriptor target_descriptor;
    target_descriptor.pixel_format = metal::pixel_format::rgba8_unorm;
    target_descriptor.width = target_descriptor.height = 64;
    target_descriptor.usage = metal::texture_usage::render_target;
    target_descriptor.options = {metal::StorageMode::private_};
    const float tint[] = {1, 1, 1, 1};
    const auto run = [&](std::uint32_t fill, std::uint32_t clip) {
        metal::RenderPass pass;
        pass.colors[0].texture = metal::Texture::create(device, target_descriptor);
        pass.colors[0].load_action = 2;
        pass.colors[0].store_action = 1;
        pass.colors[0].clear_color = {0, 0, 1, 1};
        return draw_top_half(device, pass, {pipeline}, [&](metal::CommandBuffer& commands, std::size_t) {
            commands.set_triangle_fill_mode(fill);
            commands.set_depth_clip_mode(clip);
            commands.set_bytes(metal::Stage::fragment, 0, {reinterpret_cast<const std::uint8_t*>(tint), sizeof(tint)});
        });
    };
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255}, blue{0, 0, 255, 255};
    const auto lines = run(1, 0);
    std::uint32_t lit{};
    for (std::uint32_t x = 0; x < 64; ++x) lit += pixel(lines->contents(), 256, x, 16) == red;
    expect(pixel(lines->contents(), 256, 32, 8) == blue, "line fill leaves the triangle's interior clear: " +
           format_pixel(pixel(lines->contents(), 256, 32, 8)));
    expect(lit >= 2 && lit <= 6, "line fill lights the two side edges on row 16 (" + std::to_string(lit) + " pixels)");
    const auto clamped = run(0, 1);
    expect(pixel(clamped->contents(), 256, 32, 8) == red, "the depth-clamp variant fills the triangle: " +
           format_pixel(pixel(clamped->contents(), 256, 32, 8)));
}

// setSamplerState:lodMinClamp:lodMaxClamp:atIndex: (ANGLE's per-texture
// base/max level): the binding's sampler gets the given LOD range. A
// two-level texture magnified over the triangle samples level 0 unless the
// clamp's minimum forces level 1.
void test_sampler_lod_clamp(const std::shared_ptr<metal::Device>& device) {
    metal::SamplerDescriptor mipmapped;   // nearest, mip filter nearest
    mipmapped.mip_filter = 1;
    auto sampler = metal::Sampler::create(device, mipmapped);
    expect(metal::Sampler::with_lod_clamp(sampler, 0, mipmapped.lod_max) == sampler, "the descriptor's own range is the sampler");
    const auto clamped = metal::Sampler::with_lod_clamp(sampler, 1, 1000);
    expect(clamped && clamped != sampler && clamped->descriptor.lod_min == 1 && clamped->descriptor.lod_max == 1000,
           "a clamped sampler with the new range");
    expect(metal::Sampler::with_lod_clamp(sampler, 1, 1000) == clamped, "one derived sampler per range");
    const auto inverted = metal::Sampler::with_lod_clamp(sampler, 2, 1);
    expect(inverted->descriptor.lod_min == 2 && inverted->descriptor.lod_max == 2, "an inverted range clamps to its minimum");
    expect(!metal::Sampler::with_lod_clamp(nullptr, 1, 2), "a nil sampler stays nil");
    // The cache is bounded: a range per draw restarts it instead of growing
    // without limit; samplers handed out stay usable.
    {
        auto bounded = metal::Sampler::create(device, mipmapped);
        const auto first = metal::Sampler::with_lod_clamp(bounded, 0.5F, 4);
        for (std::size_t index = 1; index < metal::Sampler::max_clamped_samplers; ++index)
            static_cast<void>(metal::Sampler::with_lod_clamp(bounded, 0.5F, 4 + static_cast<float>(index)));
        expect(metal::Sampler::with_lod_clamp(bounded, 0.5F, 4) == first, "a full cache still answers a cached range");
        static_cast<void>(metal::Sampler::with_lod_clamp(bounded, 0.25F, 4));   // one past the bound: starts over
        const auto again = metal::Sampler::with_lod_clamp(bounded, 0.5F, 4);
        expect(again != first && again->sampler != VK_NULL_HANDLE && first->sampler != VK_NULL_HANDLE,
               "past the bound the cache starts over; earlier samplers stay valid");
    }

    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(fragment_function(true)));
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    auto pipeline = metal::RenderPipeline::create(device, descriptor);
    metal::TextureDescriptor image_descriptor;
    image_descriptor.width = image_descriptor.height = 2;
    image_descriptor.mipmap_levels = 2;
    auto image = metal::Texture::create(device, image_descriptor);
    const std::uint8_t level0[] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    const std::uint8_t level1[] = {200, 100, 50, 255};
    image->replace_region({0, 0, 0, 2, 2, 1}, 0, 0, level0, 8, 0);
    image->replace_region({0, 0, 0, 1, 1, 1}, 1, 0, level1, 4, 0);
    metal::RenderPass pass;
    pass.colors[0].texture = render_target(device);
    pass.colors[0].load_action = 2;
    pass.colors[0].store_action = 1;
    pass.colors[0].clear_color = {0, 0, 1, 1};
    for (const bool clamp : {false, true}) {
        const auto readback = draw_top_half(device, pass, {pipeline}, [&](metal::CommandBuffer& commands, std::size_t) {
            commands.set_texture(metal::Stage::fragment, 0, image);
            commands.set_sampler(metal::Stage::fragment, 0, clamp ? clamped : sampler);
        });
        const auto inside = pixel(readback->contents(), 256, 32, 4);
        const std::array<std::uint8_t, 4> expected = clamp ? std::array<std::uint8_t, 4>{200, 100, 50, 255}
                                                           : std::array<std::uint8_t, 4>{0, 255, 0, 255};
        expect(inside == expected, std::string(clamp ? "lodMinClamp 1 samples level 1: " : "unclamped samples level 0: ") +
                                       format_pixel(inside));
    }
}

std::shared_ptr<metal::Texture> render_target(const std::shared_ptr<metal::Device>& device) {
    metal::TextureDescriptor descriptor;
    descriptor.pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.width = descriptor.height = 64;
    descriptor.usage = metal::texture_usage::render_target | metal::texture_usage::shader_read;
    descriptor.options = {metal::StorageMode::private_};
    return metal::Texture::create(device, descriptor);
}

std::string format_pixel(const std::array<std::uint8_t, 4>& p) {
    return std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + " " + std::to_string(p[3]);
}

// MTLMultisampleDepthResolveFilter: the top-half triangle (z = 0) drawn into a
// 4x depth target cleared to 1 and resolved into a single-sample one. Pixels
// on the triangle's slanted edges are partly covered, so their samples hold
// both 0 and 1: Min resolves them to 0, Max to 1, Sample0 to one of the two.
// Inside and outside the triangle every filter gives 0 and 1.
void test_depth_resolve_filters(const std::shared_ptr<metal::Device>& device) {
    if (!(device->profile().texture_sample_counts & 4)) {
        std::cout << "SKIP: depth resolve filters (no 4x multisampling)\n";
        return;
    }
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(fragment_function(false)));
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.depth_format = metal::pixel_format::depth32_float;
    descriptor.sample_count = 4;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    descriptor.label = "depth resolve";
    const auto pipeline = metal::RenderPipeline::create(device, descriptor);
    auto depth_state = std::make_shared<metal::DepthStencilState>();
    depth_state->depth_write = true;   // compare Always
    const auto target = [&](std::uint32_t format, std::uint32_t samples) {
        metal::TextureDescriptor texture;
        texture.type = samples > 1 ? metal::TextureType::type2d_ms : metal::TextureType::type2d;
        texture.pixel_format = format;
        texture.width = texture.height = 64;
        texture.sample_count = samples;
        texture.usage = metal::texture_usage::render_target | metal::texture_usage::shader_read;
        texture.options = {metal::StorageMode::private_};
        return metal::Texture::create(device, texture);
    };
    const float tint[] = {1, 1, 1, 1};
    const auto resolve = [&](std::uint32_t filter) {
        const auto resolved = target(metal::pixel_format::depth32_float, 1);
        metal::RenderPass pass;
        pass.colors[0].texture = target(metal::pixel_format::rgba8_unorm, 4);
        pass.colors[0].load_action = 2;
        pass.colors[0].store_action = 0;   // DontCare
        pass.depth.texture = target(metal::pixel_format::depth32_float, 4);
        pass.depth.load_action = 2;
        pass.depth.clear_depth = 1.0;
        pass.depth.store_action = 2;       // MultisampleResolve
        pass.depth.resolve_texture = resolved;
        pass.depth.resolve_filter = filter;
        const float positions[] = {-1, 1, 1, 1, 0, 0};
        auto vertices = shared_buffer(device, sizeof(positions));
        std::memcpy(vertices->contents(), positions, sizeof(positions));
        const float colors[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
        auto readback = shared_buffer(device, 64 * 256);
        auto commands = metal::CommandBuffer::create(device);
        commands->begin_render(pass);
        commands->set_render_pipeline(pipeline);
        commands->set_depth_stencil(depth_state);
        commands->set_buffer(metal::Stage::vertex, 0, vertices, 0);
        commands->set_bytes(metal::Stage::vertex, 1, {reinterpret_cast<const std::uint8_t*>(colors), sizeof(colors)});
        commands->set_bytes(metal::Stage::fragment, 0, {reinterpret_cast<const std::uint8_t*>(tint), sizeof(tint)});
        commands->draw(3, 0, 3, 1, 0);
        commands->end_encoding();
        commands->begin_blit();
        commands->copy_texture_to_buffer(resolved, 0, 0, {0, 0, 0, 64, 64, 1}, readback, 0, 256, 64 * 256);
        commands->end_encoding();
        commands->commit();
        expect(commands->wait(UINT64_MAX), "depth resolve command buffer completes");
        std::vector<float> depths(64 * 64);
        std::memcpy(depths.data(), readback->contents(), depths.size() * sizeof(float));
        return depths;
    };
    const auto sample0 = resolve(0);
    const bool min_max = (device->profile().depth_resolve_modes & (VK_RESOLVE_MODE_MIN_BIT | VK_RESOLVE_MODE_MAX_BIT)) ==
                         (VK_RESOLVE_MODE_MIN_BIT | VK_RESOLVE_MODE_MAX_BIT);
    expect(sample0[4 * 64 + 32] == 0.0F && sample0[60 * 64 + 32] == 1.0F,
           "Sample0 depth resolve: 0 inside the triangle, the clear depth 1 outside");
    if (!min_max) {
        bool refused = false;
        try { static_cast<void>(resolve(1)); } catch (const std::exception&) { refused = true; }
        expect(refused, "a Min depth resolve stops explicitly on a device without MIN/MAX resolves");
        std::cout << "SKIP: Min/Max depth resolves (the device has no MIN/MAX depth resolve modes)\n";
        return;
    }
    const auto minimum = resolve(1), maximum = resolve(2);
    std::size_t edges = 0;
    for (std::size_t index = 0; index < minimum.size(); ++index) {
        const auto lo = minimum[index], hi = maximum[index], s0 = sample0[index];
        expect(lo <= s0 && s0 <= hi, "Min <= Sample0 <= Max at every pixel");
        if (lo == 0.0F && hi == 1.0F) ++edges;
    }
    std::cout << "  depth resolve: " << edges << " partly covered edge pixels (Min 0, Max 1)\n";
    expect(edges > 0, "edge pixels resolve to 0 with Min and to 1 with Max");
    expect(minimum[4 * 64 + 32] == 0.0F && maximum[4 * 64 + 32] == 0.0F, "inside: every filter gives 0");
    expect(minimum[60 * 64 + 32] == 1.0F && maximum[60 * 64 + 32] == 1.0F, "outside: every filter gives 1");
}

// MTLVertexStepFunctionPerInstance with stepRate 2 (a vertex attribute
// divisor): four instances draw four vertical strips, and the per-instance
// colour advances every second instance: red, red, green, green.
void test_instance_step_rate(const std::shared_ptr<metal::Device>& device) {
    if (!device->vertex_attribute_divisor) {
        std::cout << "SKIP: instance step rate (no vertex attribute divisor)\n";
        return;
    }
    const auto compiled = msl::compile(R"(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { float2 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
struct VertexOut { float4 position [[position]]; float4 color; };
vertex VertexOut strips(VertexIn in [[stage_in]], uint instance [[instance_id]]) {
    VertexOut out;
    out.position = float4(in.position.x * 0.25 - 0.75 + 0.5 * float(instance), in.position.y, 0.0, 1.0);
    out.color = in.color;
    return out;
}
fragment float4 strip_colour(VertexOut in [[stage_in]]) { return in.color; }
)", {});
    expect(compiled.success(), "step-rate MSL compiles: " + msl::format_diagnostics(compiled.diagnostics));
    if (!compiled.success()) return;
    const auto function = [&](std::string_view name) {
        for (const auto& item : compiled.functions)
            if (item.name == name) return std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(item));
        throw std::runtime_error("missing function " + std::string(name));
    };
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = function("strips");
    descriptor.fragment = function("strip_colour");
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};   // float2 from buffer 0
    descriptor.attributes[1] = metal::VertexAttribute{31, 0, 2};   // float4 from buffer 2
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};          // per vertex
    descriptor.layouts[2] = metal::VertexLayout{16, 2, 2};         // per instance, step rate 2
    descriptor.label = "step rate";
    const auto pipeline = metal::RenderPipeline::create(device, descriptor);
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1};
    const float colours[] = {1, 0, 0, 1, 0, 1, 0, 1};
    auto vertices = shared_buffer(device, sizeof(quad));
    std::memcpy(vertices->contents(), quad, sizeof(quad));
    auto instances = shared_buffer(device, sizeof(colours));
    std::memcpy(instances->contents(), colours, sizeof(colours));
    metal::RenderPass pass;
    pass.colors[0].texture = render_target(device);
    pass.colors[0].load_action = 2;
    pass.colors[0].store_action = 1;
    pass.colors[0].clear_color = {0, 0, 1, 1};
    auto readback = shared_buffer(device, 64 * 256);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->set_render_pipeline(pipeline);
    commands->set_buffer(metal::Stage::vertex, 0, vertices, 0);
    commands->set_buffer(metal::Stage::vertex, 2, instances, 0);
    commands->draw(3, 0, 6, 4, 0);   // MTLPrimitiveTypeTriangle, 6 vertices, 4 instances
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_texture_to_buffer(pass.colors[0].texture, 0, 0, {0, 0, 0, 64, 64, 1}, readback, 0, 256, 64 * 256);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "step-rate command buffer completes");
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255}, green{0, 255, 0, 255};
    const std::array<std::array<std::uint8_t, 4>, 4> expected{red, red, green, green};
    for (std::uint32_t strip = 0; strip < 4; ++strip) {
        const auto p = pixel(readback->contents(), 256, 8 + 16 * strip, 32);
        expect(p == expected[strip], "instance " + std::to_string(strip) + " reads colour element " + std::to_string(strip / 2) +
                                         ": " + format_pixel(p));
    }
}

// MTLBufferLayoutStrideDynamic: the vertex buffer's stride comes with the bind
// (setVertexBuffer:offset:attributeStride:atIndex:). Positions sit 16 bytes
// apart with a decoy pair between them, so the top wedge (-1,1) (1,1) (0,0)
// appears only when the bind's stride is used; an 8-byte stride would draw
// (-1,1) (-1,-1) (1,1), which covers the left pixel.
void test_dynamic_vertex_stride(const std::shared_ptr<metal::Device>& device) {
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(fragment_function(false)));
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{metal::dynamic_vertex_stride, 1, 1};
    descriptor.label = "dynamic stride";
    const auto pipeline = metal::RenderPipeline::create(device, descriptor);
    expect(pipeline->dynamic_strides, "a MTLBufferLayoutStrideDynamic layout makes a dynamic-stride pipeline");
    const float positions[] = {-1, 1, -1, -1, 1, 1, -1, -1, 0, 0, -1, -1};
    auto vertices = shared_buffer(device, sizeof(positions));
    std::memcpy(vertices->contents(), positions, sizeof(positions));
    const float colors[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    const float tint[] = {1, 1, 1, 1};
    metal::RenderPass pass;
    pass.colors[0].texture = render_target(device);
    pass.colors[0].load_action = 2;
    pass.colors[0].store_action = 1;
    pass.colors[0].clear_color = {0, 0, 1, 1};
    auto readback = shared_buffer(device, 64 * 256);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->set_render_pipeline(pipeline);
    commands->set_buffer(metal::Stage::vertex, 0, vertices, 0);
    commands->set_attribute_stride(metal::Stage::vertex, 0, 16);
    commands->set_bytes(metal::Stage::vertex, 1, {reinterpret_cast<const std::uint8_t*>(colors), sizeof(colors)});
    commands->set_bytes(metal::Stage::fragment, 0, {reinterpret_cast<const std::uint8_t*>(tint), sizeof(tint)});
    commands->draw(3, 0, 3, 1, 0);
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_texture_to_buffer(pass.colors[0].texture, 0, 0, {0, 0, 0, 64, 64, 1}, readback, 0, 256, 64 * 256);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "dynamic-stride command buffer completes");
    const auto top = pixel(readback->contents(), 256, 32, 4), left = pixel(readback->contents(), 256, 4, 28);
    expect(top == std::array<std::uint8_t, 4>{255, 0, 0, 255}, "the wedge drawn with the bind's stride: " + format_pixel(top));
    expect(left == std::array<std::uint8_t, 4>{0, 0, 255, 255}, "and not the 8-byte-stride triangle: " + format_pixel(left));
}

// Vector [[function_constant]]s (float4, int3, bool2): every component is
// its own specialization constant, filled from the MTLFunctionConstantValues
// bytes (MSL sizes: an int3 value takes 16 bytes).
void test_vector_function_constants(const std::shared_ptr<metal::Device>& device) {
    expect(metal::data_type_size(6) == 16 && metal::data_type_size(31) == 16 && metal::data_type_size(54) == 2 &&
               metal::data_type_size(18) == 8,
           "MTLDataType Float4/Int3/Bool2/Half3 sizes");
    const auto compiled = msl::compile(R"(
#include <metal_stdlib>
using namespace metal;
constant float4 scale [[function_constant(0)]];
constant int3 offsets [[function_constant(1)]];
constant bool2 flags [[function_constant(2)]];
kernel void vector_constants(device float* out [[buffer(0)]], uint id [[thread_position_in_grid]]) {
    out[id] = scale.x * float(id) + scale.w + float(offsets.z) + (flags.y ? 1000.0f : 0.0f) + (flags.x ? 1.0e6f : 0.0f);
}
)", {});
    expect(compiled.success(), "vector function constant MSL compiles: " + msl::format_diagnostics(compiled.diagnostics));
    if (!compiled.success()) return;
    auto shader = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(compiled.functions.at(0)));
    const auto bytes = [](const auto& value) {
        const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
        return std::vector<std::uint8_t>(first, first + sizeof(value));
    };
    const float scale[4] = {2, 9, 9, 5};
    const std::int32_t offsets[4] = {7, 8, 20, 0};
    const std::uint8_t flags[2] = {0, 1};
    metal::ConstantValues values;
    values.by_index[0] = {6, bytes(scale)};
    values.by_index[1] = {31, bytes(offsets)};
    values.by_name["flags"] = {54, bytes(flags)};
    const auto failure = shader->specialize(values);
    expect(!failure, "vector function constants specialize: " + failure.value_or(""));
    if (failure) return;
    const auto pipeline = metal::ComputePipeline::create(device, shader);
    auto out = shared_buffer(device, 16);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_compute(false);
    commands->set_compute_pipeline(pipeline);
    commands->set_buffer(metal::Stage::kernel, 0, out, 0);
    commands->dispatch_threads({4, 1, 1}, {4, 1, 1});
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "vector function constant kernel completes");
    const auto* result = reinterpret_cast<const float*>(out->contents());
    bool correct = true;
    for (std::uint32_t i = 0; i < 4; ++i) correct &= result[i] == 2.0F * static_cast<float>(i) + 1025.0F;
    expect(correct, "scale.x, scale.w, offsets.z and flags.y reach the kernel: " + std::to_string(result[0]) + " " +
                        std::to_string(result[3]));
}

// A texture buffer, a view of it in another format of the same texel size,
// and texture-to-texture copies between texture buffers and a 2D texture
// (one row): the bytes arrive where Metal puts them.
void test_texture_buffer_views_and_copies(const std::shared_ptr<metal::Device>& device) {
    auto backing = shared_buffer(device, 256);
    auto* bytes = static_cast<std::uint8_t*>(backing->contents());
    for (std::uint32_t i = 0; i < 256; ++i) bytes[i] = static_cast<std::uint8_t>(i);
    metal::TextureDescriptor d;
    d.type = metal::TextureType::texture_buffer;
    d.pixel_format = metal::pixel_format::rgba8_unorm;
    d.width = 16;
    d.options = {metal::StorageMode::shared};
    const auto texels = metal::Texture::create_buffer_texture(backing, d, 0, 64);
    constexpr std::uint32_t r32_uint = 53;   // MTLPixelFormatR32Uint
    const auto view = metal::Texture::create_view(texels, r32_uint, metal::TextureType::texture_buffer, 0, 1, 0, 1, {2, 3, 4, 5});
    expect(view->buffer_view && view->descriptor.width == 16 && view->buffer == backing,
           "an R32Uint view of an RGBA8 texture buffer is a texel buffer over the same bytes");
    bool refused{};
    try { static_cast<void>(metal::Texture::create_view(texels, r32_uint, metal::TextureType::texture_buffer, 0, 1, 0, 1, {5, 4, 3, 2})); }
    catch (const std::exception&) { refused = true; }
    expect(refused, "a swizzled texture buffer view stops explicitly");

    metal::TextureDescriptor image;
    image.pixel_format = metal::pixel_format::rgba8_unorm;
    image.width = 16;
    image.height = 4;
    image.usage = metal::texture_usage::shader_read;
    image.options = {metal::StorageMode::private_};
    const auto texture = metal::Texture::create(device, image);
    auto out = shared_buffer(device, 256);
    std::memset(out->contents(), 0xee, 256);
    d.width = 16;
    const auto target = metal::Texture::create_buffer_texture(out, d, 0, 64);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    // Texels 2..9 of the buffer -> row 1 of the image at x 4; then row 1,
    // x 4..11 of the image -> the second texture buffer at texel 3; then
    // texels 0..1 of the first texture buffer -> the second at texel 14.
    commands->copy_texture(texels, 0, 0, {2, 0, 0, 8, 1, 1}, texture, 0, 0, 4, 1, 0);
    commands->copy_texture(texture, 0, 0, {4, 1, 0, 8, 1, 1}, target, 0, 0, 3, 0, 0);
    commands->copy_texture(texels, 0, 0, {0, 0, 0, 2, 1, 1}, target, 0, 0, 14, 0, 0);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "texture buffer copies complete");
    const auto* result = static_cast<const std::uint8_t*>(out->contents());
    bool through_image = true, direct = true, untouched = true;
    for (std::uint32_t i = 0; i < 32; ++i) through_image &= result[12 + i] == static_cast<std::uint8_t>(8 + i);
    for (std::uint32_t i = 0; i < 8; ++i) direct &= result[56 + i] == static_cast<std::uint8_t>(i);
    for (std::uint32_t i = 0; i < 12; ++i) untouched &= result[i] == 0xee;
    expect(through_image, "texture buffer -> texture -> texture buffer keeps texels 2..9 at texel 3");
    expect(direct, "texture buffer -> texture buffer copies texels 0..1 to texel 14");
    expect(untouched, "texels before the copied range are untouched");
}

// MTLHeapTypePlacement: buffers placed at the same offset alias (a GPU fill
// through one is read through the other), a texture placed at its alignment
// round-trips through blits, and a misaligned offset stops explicitly.
void test_placement_heap(const std::shared_ptr<metal::Device>& device) {
    metal::Heap heap(device, 1 << 20, {metal::StorageMode::shared}, 1);
    expect(heap.placement && heap.placement->allocation, "a placement heap owns one allocation");
    const auto buffer_alignment = metal::placed_buffer_requirements(*device, 256).alignment;
    const auto a = metal::Buffer::create_placed(heap.placement, buffer_alignment, 256, {metal::StorageMode::shared});
    const auto b = metal::Buffer::create_placed(heap.placement, buffer_alignment, 256, {metal::StorageMode::shared});
    metal::TextureDescriptor image;
    image.pixel_format = metal::pixel_format::rgba8_unorm;
    image.width = image.height = 16;
    image.options = {metal::StorageMode::shared};
    const auto texture_requirements = metal::Texture::placed_requirements(device, image);
    const auto texture_offset = (512 * 1024 + texture_requirements.alignment - 1) / texture_requirements.alignment *
                                texture_requirements.alignment;
    const auto texture = metal::Texture::create(device, image, heap.placement, texture_offset);
    std::cout << "  placement heap: buffer alignment " << buffer_alignment << ", texture size " << texture_requirements.size
              << " alignment " << texture_requirements.alignment << "\n";
    auto source = shared_buffer(device, 1024);
    for (std::uint32_t i = 0; i < 1024; ++i) static_cast<std::uint8_t*>(source->contents())[i] = static_cast<std::uint8_t>(i * 7);
    auto readback = shared_buffer(device, 1024);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    commands->fill_buffer(a, 0, 256, 0x5a);
    commands->copy_buffer_to_texture(source, 0, 64, 1024, {0, 0, 0, 16, 16, 1}, texture, 0, 0, 0);
    commands->copy_texture_to_buffer(texture, 0, 0, {0, 0, 0, 16, 16, 1}, readback, 0, 64, 1024);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "placement heap command buffer completes");
    const auto* through_b = b->contents();
    bool aliased = through_b != nullptr;
    for (std::uint32_t i = 0; aliased && i < 256; ++i) aliased = through_b[i] == 0x5a;
    expect(aliased, "a fill through one placed buffer is read through another at the same offset");
    expect(std::memcmp(readback->contents(), source->contents(), 1024) == 0, "a placed texture round-trips through blits");
    bool refused{};
    try { static_cast<void>(metal::Buffer::create_placed(heap.placement, buffer_alignment + 4, 16, {metal::StorageMode::shared})); }
    catch (const std::exception&) { refused = true; }
    expect(refused, "a misaligned placement offset stops explicitly");
}

// newTextureWithDescriptor:offset:bytesPerRow: with MTLTextureType2D: a
// linear image over the buffer's own memory. The buffer's bytes are the
// texture's texels, and a blit into the texture changes the buffer.
// Tessellation (M5): a post-tessellation vertex function with control-point
// and per-patch attributes, per-patch factors (0 culls a patch), an indirect
// drawPatches, and drawIndexedPatches through patch and control point index
// buffers.
void test_tessellation(const std::shared_ptr<metal::Device>& device) {
    const auto compiled = msl::compile(R"(
#include <metal_stdlib>
using namespace metal;
struct ControlPoint { float2 position [[attribute(0)]]; };
struct PatchIn { patch_control_point<ControlPoint> points; float4 color [[attribute(1)]]; };
struct Out { float4 position [[position]]; float4 color; };
[[patch(quad, 4)]]
vertex Out post(PatchIn in [[stage_in]], float2 uv [[position_in_patch]]) {
    const auto p = in.points;
    Out out;
    out.position = float4(mix(mix(p[0].position, p[1].position, uv.x), mix(p[3].position, p[2].position, uv.x), uv.y), 0, 1);
    out.color = float4(in.color.r, uv.x, in.color.b, 1);
    return out;
}
fragment float4 color(Out in [[stage_in]]) { return in.color; }
)", {});
    expect(compiled.success(), "tessellation MSL compiles: " + msl::format_diagnostics(compiled.diagnostics));
    if (!compiled.success()) return;
    const auto find = [&](const char* name) {
        for (const auto& function : compiled.functions)
            if (function.name == name)
                return std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(function));
        return std::shared_ptr<metal::ShaderFunction>();
    };
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = find("post");
    descriptor.fragment = find("color");
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 1};   // Float2 control points
    descriptor.layouts[1] = metal::VertexLayout{8, 4, 1};          // PerPatchControlPoint
    descriptor.attributes[1] = metal::VertexAttribute{31, 0, 2};   // Float4 colours
    descriptor.layouts[2] = metal::VertexLayout{16, 3, 1};         // PerPatch
    descriptor.tessellation.partition = 1;                          // Integer
    descriptor.tessellation.factor_step = 1;                        // PerPatch
    descriptor.label = "tessellation";
    if (!device->features10.tessellationShader) {
        bool refused = false;
        try { static_cast<void>(metal::RenderPipeline::create(device, descriptor)); } catch (const std::runtime_error&) { refused = true; }
        expect(refused, "without tessellation shaders a post-tessellation pipeline is refused");
        return;
    }
    const auto direct = metal::RenderPipeline::create(device, descriptor);
    descriptor.tessellation.control_point_index_type = 1;           // UInt16
    const auto indexed = metal::RenderPipeline::create(device, descriptor);
    expect(direct->tessellation && direct->control_points == 4 && direct->tessellation_buffers.size() == 2,
           "a [[patch]] vertex function makes a tessellation pipeline");

    // Patch 0: the left half; patch 1: the right half (factor 0: culled);
    // patch 2 (indexed only): the top-right quadrant.
    const float points[] = {-1, -1, 0, -1, 0, 1, -1, 1, 0, -1, 1, -1, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1};
    const float colors[] = {1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1};
    const std::uint16_t four = 0x4400, one = 0x3C00;   // half 4.0 and 1.0
    const std::uint16_t factors[] = {four, four, four, four, four, four, 0, 0, 0, 0, 0, 0, one, one, one, one, one, one};
    const std::uint32_t indirect_arguments[] = {2, 1, 0, 0};   // patchCount, instanceCount, patchStart, baseInstance
    const std::uint32_t patch_indices[] = {2};
    const std::uint16_t control_point_indices[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    const auto upload = [&](const void* data, std::size_t size) {
        auto buffer = shared_buffer(device, size);
        std::memcpy(buffer->contents(), data, size);
        return buffer;
    };
    const auto point_buffer = upload(points, sizeof(points)), color_buffer = upload(colors, sizeof(colors));
    const auto factor_buffer = upload(factors, sizeof(factors)), indirect = upload(indirect_arguments, sizeof(indirect_arguments));
    const auto patch_buffer = upload(patch_indices, sizeof(patch_indices));
    const auto control_buffer = upload(control_point_indices, sizeof(control_point_indices));
    metal::RenderPass pass;
    pass.colors[0].texture = render_target(device);
    pass.colors[0].load_action = 2;
    pass.colors[0].store_action = 1;
    pass.colors[0].clear_color = {0, 0, 1, 1};
    auto readback = shared_buffer(device, 64 * 256);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->set_render_pipeline(direct);
    commands->set_buffer(metal::Stage::vertex, 1, point_buffer, 0);
    commands->set_buffer(metal::Stage::vertex, 2, color_buffer, 0);
    commands->set_tessellation_factor_buffer(factor_buffer, 0, 0);
    metal::CommandBuffer::PatchDraw draw;
    draw.control_points = 4;
    draw.indirect = indirect;
    commands->draw_patches(draw);
    commands->set_render_pipeline(indexed);
    draw = {};
    draw.control_points = 4;
    draw.patch_count = 1;
    draw.patch_indices = patch_buffer;
    draw.indexed = true;
    draw.control_point_indices = control_buffer;
    commands->draw_patches(draw);
    bool refused = false;
    try { commands->draw(3, 0, 3, 1, 0); } catch (const std::runtime_error&) { refused = true; }
    expect(refused, "drawPrimitives with a post-tessellation pipeline is refused");
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_texture_to_buffer(pass.colors[0].texture, 0, 0, {0, 0, 0, 64, 64, 1}, readback, 0, 256, 64 * 256);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "tessellation command buffer completes");
    const auto near = [](std::uint8_t value, int expected) { return std::abs(static_cast<int>(value) - expected) <= 10; };
    const auto left = pixel(readback->contents(), 256, 4, 32), middle = pixel(readback->contents(), 256, 28, 32);
    expect(left[0] == 255 && near(left[1], 36) && left[2] == 0, "patch 0, u near 0: " + format_pixel(left));
    expect(middle[0] == 255 && near(middle[1], 227) && middle[2] == 0, "patch 0, u near 1: " + format_pixel(middle));
    const auto culled = pixel(readback->contents(), 256, 48, 48), indexed_patch = pixel(readback->contents(), 256, 48, 16);
    expect(culled == std::array<std::uint8_t, 4>{0, 0, 255, 255}, "factor 0 culls patch 1: " + format_pixel(culled));
    expect(indexed_patch[0] == 255 && indexed_patch[2] == 0, "patch 2 through the index buffers: " + format_pixel(indexed_patch));
}
// Indirect command buffers (M6): CPU-encoded render and compute commands
// with their own pipelines and buffers, reset and copy, replayed by
// execute_commands; the encoder's own state applies again afterwards.
void test_indirect_command_buffers(const std::shared_ptr<metal::Device>& device) {
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(fragment_function(false)));
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    descriptor.label = "indirect commands";
    const auto pipeline = metal::RenderPipeline::create(device, descriptor);
    const auto upload = [&](std::initializer_list<float> values) {
        auto buffer = shared_buffer(device, values.size() * 4);
        std::memcpy(buffer->contents(), values.begin(), values.size() * 4);
        return buffer;
    };
    const auto top = upload({-1, 1, 1, 1, 0, 0}), left = upload({-1, 1, -1, -1, 0, 0}), bottom = upload({-1, -1, 1, -1, 0, 0});
    const auto right = upload({1, 1, 1, -1, 0, 0});
    const auto red = upload({1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1}), green = upload({0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1});
    const auto white = upload({1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}), tint = upload({1, 1, 1, 1});
    auto commands_buffer = std::make_shared<metal::IndirectCommandBuffer>(1 | 2, 3, false, false);
    auto other = std::make_shared<metal::IndirectCommandBuffer>(1 | 2, 1, false, false);
    const auto encode = [&](metal::IndirectRenderCommand& command, const std::shared_ptr<metal::Buffer>& positions,
                            const std::shared_ptr<metal::Buffer>& colors) {
        command.kind = metal::IndirectRenderCommand::Kind::draw;
        command.pipeline = pipeline;
        command.vertex[0] = {positions, 0};
        command.vertex[1] = {colors, 0};
        command.fragment[0] = {tint, 0};
        command.primitive = 3;
        command.count = 3;
        command.instance_count = 1;
    };
    encode(commands_buffer->render[0], top, red);
    encode(commands_buffer->render[1], left, green);
    commands_buffer->copy_to(*other, 1, 1, 0);
    encode(commands_buffer->render[1], right, green);   // then reset: draws nothing
    commands_buffer->reset(1, 1);
    metal::RenderPass pass;
    pass.colors[0].texture = render_target(device);
    pass.colors[0].load_action = 2;
    pass.colors[0].store_action = 1;
    pass.colors[0].clear_color = {0, 0, 0, 1};
    auto readback = shared_buffer(device, 64 * 256);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->set_render_pipeline(pipeline);
    commands->set_buffer(metal::Stage::vertex, 0, bottom, 0);
    commands->set_buffer(metal::Stage::vertex, 1, white, 0);
    commands->set_buffer(metal::Stage::fragment, 0, tint, 0);
    commands->execute_commands(*commands_buffer, 0, 3);
    commands->execute_commands(*other, 0, 1);
    commands->draw(3, 0, 3, 1, 0);   // the encoder's own buffers again
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_texture_to_buffer(pass.colors[0].texture, 0, 0, {0, 0, 0, 64, 64, 1}, readback, 0, 256, 64 * 256);
    commands->end_encoding();

    // Compute: two dispatches with their own buffers, the second behind a barrier.
    const auto compiled = msl::compile(R"(
#include <metal_stdlib>
using namespace metal;
kernel void fill(device uint* out [[buffer(0)]], constant uint& base [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    out[id] = base + id;
}
)", {});
    expect(compiled.success(), "indirect compute MSL compiles: " + msl::format_diagnostics(compiled.diagnostics));
    if (!compiled.success()) return;
    const auto kernel = metal::ComputePipeline::create(device, std::make_shared<metal::ShaderFunction>(device,
        std::make_shared<msl::Function>(compiled.functions.at(0))));
    auto out = shared_buffer(device, 32);
    const std::uint32_t seven = 7, hundred = 100;
    auto first_base = shared_buffer(device, 4), second_base = shared_buffer(device, 4);
    std::memcpy(first_base->contents(), &seven, 4);
    std::memcpy(second_base->contents(), &hundred, 4);
    metal::IndirectCommandBuffer dispatches(32 | 64, 2, false, false);
    auto& first = dispatches.compute[0];
    first.kind = metal::IndirectComputeCommand::Kind::threads;
    first.pipeline = kernel;
    first.buffers[0] = {out, 0};
    first.buffers[1] = {first_base, 0};
    first.grid = {4, 1, 1};
    first.threads = {4, 1, 1};
    auto& second = dispatches.compute[1];
    second = first;
    second.kind = metal::IndirectComputeCommand::Kind::threadgroups;
    second.buffers[0] = {out, 16};
    second.buffers[1] = {second_base, 0};
    second.grid = {1, 1, 1};
    second.barrier = true;
    commands->begin_compute(false);
    commands->execute_commands(dispatches, 0, 2);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "indirect command buffer completes");
    const auto at = [&](std::uint32_t x, std::uint32_t y) { return pixel(readback->contents(), 256, x, y); };
    expect(at(32, 4) == std::array<std::uint8_t, 4>{255, 0, 0, 255}, "command 0 draws red: " + format_pixel(at(32, 4)));
    expect(at(4, 32) == std::array<std::uint8_t, 4>{0, 255, 0, 255}, "the copied command draws green: " + format_pixel(at(4, 32)));
    expect(at(32, 60) == std::array<std::uint8_t, 4>{255, 255, 255, 255},
           "the encoder's own draw after the commands: " + format_pixel(at(32, 60)));
    expect(at(60, 32) == std::array<std::uint8_t, 4>{0, 0, 0, 255}, "the reset command draws nothing: " + format_pixel(at(60, 32)));
    const auto* values = reinterpret_cast<const std::uint32_t*>(out->contents());
    bool filled = true;
    for (std::uint32_t i = 0; i < 4; ++i) filled &= values[i] == 7 + i && values[4 + i] == 100 + i;
    expect(filled, "indirect compute commands use their own buffers: " + std::to_string(values[0]) + " " + std::to_string(values[4]));
}
void test_linear_texture_over_buffer(const std::shared_ptr<metal::Device>& device) {
    const auto alignment = metal::Texture::linear_alignment(device, metal::pixel_format::rgba8_unorm);
    std::cout << "  linear RGBA8: row pitch alignment " << alignment.row << ", offset alignment " << alignment.offset << "\n";
    if (!alignment.row) {
        std::cout << "SKIP: linear texture over a buffer (no linear RGBA8 images)\n";
        return;
    }
    constexpr std::uint32_t width = 16, height = 8;
    const auto row = (std::uint64_t{width} * 4 + alignment.row - 1) / alignment.row * alignment.row;
    auto backing = shared_buffer(device, 65536);   // 64 KiB and more: on the linear offset alignment
    auto* bytes = static_cast<std::uint8_t*>(backing->contents());
    for (std::uint64_t i = 0; i < row * height; ++i) bytes[i] = static_cast<std::uint8_t>(i * 3 + 1);
    metal::TextureDescriptor d;
    d.type = metal::TextureType::type2d;
    d.pixel_format = metal::pixel_format::rgba8_unorm;
    d.width = width;
    d.height = height;
    d.usage = metal::texture_usage::shader_read;
    d.options = {metal::StorageMode::shared};
    std::shared_ptr<metal::Texture> texture;
    try {
        texture = metal::Texture::create_buffer_texture(backing, d, 0, row);
    } catch (const std::exception& error) {
        expect(false, std::string("a linear texture over a buffer: ") + error.what());
        return;
    }
    auto readback = shared_buffer(device, width * 4 * height);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    commands->copy_texture_to_buffer(texture, 0, 0, {0, 0, 0, width, height, 1}, readback, 0, width * 4, width * 4 * height);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "linear texture readback completes");
    bool same = true;
    for (std::uint32_t y = 0; y < height; ++y)
        same &= std::memcmp(static_cast<const std::uint8_t*>(readback->contents()) + y * width * 4, bytes + y * row, width * 4) == 0;
    expect(same, "the texture's texels are the buffer's bytes, row by row at bytesPerRow");

    auto source = shared_buffer(device, 64);
    std::memset(source->contents(), 0xc3, 64);
    commands = metal::CommandBuffer::create(device);
    commands->begin_blit();
    commands->copy_buffer_to_texture(source, 0, 64, 64, {0, 2, 0, 16, 1, 1}, texture, 0, 0, 0);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "linear texture write completes");
    bool written = true;
    for (std::uint32_t i = 0; i < 64; ++i) written &= bytes[2 * row + i] == 0xc3;
    expect(written && bytes[row] != 0xc3, "a blit into the texture writes row 2 of the buffer");

    bool refused{};
    try { static_cast<void>(metal::Texture::create_buffer_texture(backing, d, 0, row + alignment.row)); }
    catch (const std::exception&) { refused = true; }
    expect(refused, "a bytesPerRow other than the driver's row pitch stops explicitly");
}

// A multisample resolve asked for after the pass began
// (setColorStoreAction:MultisampleResolve on an encoder made with
// MTLStoreActionUnknown): the resolve target gets the triangle over the clear
// colour, as with a resolve given at creation, and edge pixels are blends.
void test_deferred_resolve(const std::shared_ptr<metal::Device>& device) {
    if (!(device->profile().texture_sample_counts & 4)) {
        std::cout << "SKIP: deferred resolve (no 4x multisampling)\n";
        return;
    }
    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(fragment_function(false)));
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.sample_count = 4;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    descriptor.label = "deferred resolve";
    const auto pipeline = metal::RenderPipeline::create(device, descriptor);
    metal::TextureDescriptor ms;
    ms.type = metal::TextureType::type2d_ms;
    ms.pixel_format = metal::pixel_format::rgba8_unorm;
    ms.width = ms.height = 64;
    ms.sample_count = 4;
    ms.usage = metal::texture_usage::render_target;
    ms.options = {metal::StorageMode::private_};
    metal::RenderPass pass;
    pass.colors[0].texture = metal::Texture::create(device, ms);
    pass.colors[0].load_action = 2;
    pass.colors[0].store_action = 4;   // MTLStoreActionUnknown
    pass.colors[0].clear_color = {0, 0, 1, 1};
    const auto resolved = render_target(device);
    pass.colors[0].resolve_texture = resolved;
    const float positions[] = {-1, 1, 1, 1, 0, 0};
    auto vertices = shared_buffer(device, sizeof(positions));
    std::memcpy(vertices->contents(), positions, sizeof(positions));
    const float colors[] = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    const float tint[] = {1, 1, 1, 1};
    auto readback = shared_buffer(device, 64 * 256);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_render(pass);
    commands->set_render_pipeline(pipeline);
    commands->set_buffer(metal::Stage::vertex, 0, vertices, 0);
    commands->set_bytes(metal::Stage::vertex, 1, {reinterpret_cast<const std::uint8_t*>(colors), sizeof(colors)});
    commands->set_bytes(metal::Stage::fragment, 0, {reinterpret_cast<const std::uint8_t*>(tint), sizeof(tint)});
    commands->draw(3, 0, 3, 1, 0);
    commands->set_store_action(0, 2);   // MTLStoreActionMultisampleResolve, after the draw
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_texture_to_buffer(resolved, 0, 0, {0, 0, 0, 64, 64, 1}, readback, 0, 256, 64 * 256);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "deferred resolve command buffer completes");
    const auto inside = pixel(readback->contents(), 256, 32, 4), outside = pixel(readback->contents(), 256, 32, 60);
    expect(inside == std::array<std::uint8_t, 4>{255, 0, 0, 255}, "the deferred resolve has the triangle: " + format_pixel(inside));
    expect(outside == std::array<std::uint8_t, 4>{0, 0, 255, 255}, "and the clear colour outside: " + format_pixel(outside));
    bool blended = false;
    for (std::uint32_t y = 0; y < 32 && !blended; ++y)
        for (std::uint32_t x = 0; x < 64 && !blended; ++x) {
            const auto p = pixel(readback->contents(), 256, x, y);
            blended = p[0] > 20 && p[0] < 235 && p[2] > 20 && p[2] < 235;
        }
    expect(blended, "edge pixels are resolved blends of the samples");
}

// [[barycentric_coord]] from MSL source (perspective and center_no_perspective):
// the top-half triangle's fragments output their barycentrics as colour, so
// the pixel next to each vertex is nearly pure red, green and blue. Skipped
// without VK_KHR_fragment_shader_barycentric (supportsShaderBarycentricCoordinates NO).
void test_barycentric_coordinates(const std::shared_ptr<metal::Device>& device) {
    if (!device->fragment_shader_barycentric) {
        std::cout << "SKIP: barycentric coordinates (no VK_KHR_fragment_shader_barycentric)\n";
        return;
    }
    const auto compiled = msl::compile(R"(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { float2 position [[attribute(0)]]; };
struct VertexOut { float4 position [[position]]; };
vertex VertexOut bary_vertex(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    return out;
}
fragment float4 bary_perspective(VertexOut in [[stage_in]], float3 b [[barycentric_coord]]) { return float4(b, 1.0); }
struct Linear { float4 position [[position]]; float3 b [[barycentric_coord, center_no_perspective]]; };
fragment float4 bary_linear(Linear in [[stage_in]]) { return float4(in.b, 1.0); }
)", {});
    expect(compiled.success(), "barycentric MSL compiles: " + msl::format_diagnostics(compiled.diagnostics));
    if (!compiled.success()) return;
    const auto function = [&](std::string_view name) {
        for (const auto& item : compiled.functions)
            if (item.name == name) return std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(item));
        throw std::runtime_error("missing function " + std::string(name));
    };
    for (const auto* fragment : {"bary_perspective", "bary_linear"}) {
        metal::RenderPipelineDescriptor descriptor;
        descriptor.vertex = function("bary_vertex");
        descriptor.fragment = function(fragment);
        descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
        descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
        descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
        descriptor.label = fragment;
        const auto pipeline = metal::RenderPipeline::create(device, descriptor);
        metal::RenderPass pass;
        pass.colors[0].texture = render_target(device);
        pass.colors[0].load_action = 2;
        pass.colors[0].store_action = 1;
        pass.colors[0].clear_color = {0, 0, 0, 1};
        const auto readback = draw_top_half(device, pass, {pipeline}, [](metal::CommandBuffer&, std::size_t) {});
        // Vertices: (-1, 1) top left, (1, 1) top right, (0, 0) the centre.
        const auto left = pixel(readback->contents(), 256, 2, 1), right = pixel(readback->contents(), 256, 61, 1),
                   centre = pixel(readback->contents(), 256, 32, 30);
        const auto dominant = [](const std::array<std::uint8_t, 4>& p, int channel) {
            for (int c = 0; c < 3; ++c)
                if (c == channel ? p[c] < 200 : p[c] > 60) return false;
            return true;
        };
        expect(dominant(left, 0) && dominant(right, 1) && dominant(centre, 2),
               std::string(fragment) + ": barycentrics weight the nearest vertex: " + format_pixel(left) + " / " +
                   format_pixel(right) + " / " + format_pixel(centre));
    }
}

// Argument buffers holding textures and samplers (tier 2, bindless.hpp): a
// fragment samples one of two 1x1 textures (red, green) picked at run time
// from an argument buffer that also holds the sampler, by MTLResourceID; a
// kernel writes a storage texture named in an argument buffer.
void test_bindless_argument_buffers(const std::shared_ptr<metal::Device>& device) {
    if (!metal::Bindless::of(*device)) {
        std::cout << "SKIP: argument buffers with textures (no bindless descriptor set on this device)\n";
        return;
    }
    const auto compiled = msl::compile(R"(
#include <metal_stdlib>
using namespace metal;
struct VertexIn { float2 position [[attribute(0)]]; };
struct VertexOut { float4 position [[position]]; };
vertex VertexOut ab_vertex(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    return out;
}
struct Textures {
    array<texture2d<float>, 2> images [[id(0)]];
    sampler nearest [[id(2)]];
};
fragment float4 ab_pick(VertexOut in [[stage_in]], constant Textures& t [[buffer(0)]], constant uint& which [[buffer(1)]]) {
    return t.images[which].sample(t.nearest, float2(0.5));
}
struct Target { texture2d<float, access::write> image [[id(0)]]; };
kernel void ab_fill(constant Target& target [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    target.image.write(float4(0.25, 0.5, 0.75, 1.0), gid);
}
)", {});
    expect(compiled.success(), "bindless MSL compiles: " + msl::format_diagnostics(compiled.diagnostics));
    if (!compiled.success()) return;
    const auto function = [&](std::string_view name) {
        for (const auto& item : compiled.functions)
            if (item.name == name) return std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(item));
        throw std::runtime_error("missing function " + std::string(name));
    };
    for (const auto& item : compiled.functions)
        expect(item.name == "ab_vertex" || item.reflection.bindless, item.name + " reads the bindless set");

    // Two 1x1 textures and their argument buffer: images[0], images[1], the sampler.
    const auto texel = [&](std::array<std::uint8_t, 4> rgba) {
        metal::TextureDescriptor d;
        d.width = d.height = 1;
        auto texture = metal::Texture::create(device, d);
        texture->replace_region({0, 0, 0, 1, 1, 1}, 0, 0, std::span<const std::uint8_t>(rgba.data(), rgba.size()), 4, 0);
        return texture;
    };
    const auto red = texel({255, 0, 0, 255}), green = texel({0, 255, 0, 255});
    const auto sampler = metal::Sampler::create(device, metal::SamplerDescriptor{});
    const std::uint64_t ids[3] = {red->bindless_slot(), green->bindless_slot(), metal::sampler_token(sampler)};
    expect(ids[0] && ids[1] && ids[0] != ids[1] && ids[2], "textures and the sampler get bindless slots");
    const auto arguments = shared_buffer(device, sizeof(ids));
    std::memcpy(arguments->contents(), ids, sizeof(ids));

    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = function("ab_vertex");
    descriptor.fragment = function("ab_pick");
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    descriptor.label = "bindless pick";
    const auto pipeline = metal::RenderPipeline::create(device, descriptor);
    expect(pipeline->bindless, "the pipeline layout has the bindless set");
    for (const std::uint32_t which : {1U, 0U}) {
        metal::RenderPass pass;
        pass.colors[0].texture = render_target(device);
        pass.colors[0].load_action = 2;
        pass.colors[0].store_action = 1;
        pass.colors[0].clear_color = {0, 0, 1, 1};
        const auto readback = draw_top_half(device, pass, {pipeline}, [&](metal::CommandBuffer& commands, std::size_t) {
            commands.set_buffer(metal::Stage::fragment, 0, arguments, 0);
            commands.set_bytes(metal::Stage::fragment, 1, {reinterpret_cast<const std::uint8_t*>(&which), sizeof(which)});
        });
        const auto inside = pixel(readback->contents(), 256, 32, 4), outside = pixel(readback->contents(), 256, 32, 60);
        const std::array<std::uint8_t, 4> expected = which ? std::array<std::uint8_t, 4>{0, 255, 0, 255}
                                                           : std::array<std::uint8_t, 4>{255, 0, 0, 255};
        expect(inside == expected, "the argument buffer's texture " + std::to_string(which) + " is sampled: " + format_pixel(inside));
        expect(outside == std::array<std::uint8_t, 4>{0, 0, 255, 255}, "clear colour outside: " + format_pixel(outside));
    }

    // A storage texture named in an argument buffer, written by a kernel.
    metal::TextureDescriptor target_descriptor;
    target_descriptor.width = target_descriptor.height = 4;
    target_descriptor.usage = metal::texture_usage::shader_read | metal::texture_usage::shader_write;
    target_descriptor.options = {metal::StorageMode::private_};
    const auto target = metal::Texture::create(device, target_descriptor);
    const std::uint64_t target_id = target->bindless_slot();
    expect(target_id != 0, "the storage texture gets a bindless slot");
    const auto target_arguments = shared_buffer(device, 8);
    std::memcpy(target_arguments->contents(), &target_id, 8);
    const auto kernel = metal::ComputePipeline::create(device, function("ab_fill"));
    expect(kernel->bindless, "the kernel's layout has the bindless set");
    auto readback = shared_buffer(device, 4 * 256);
    auto commands = metal::CommandBuffer::create(device);
    commands->begin_compute(false);
    commands->set_compute_pipeline(kernel);
    commands->set_buffer(metal::Stage::kernel, 0, target_arguments, 0);
    commands->dispatch_threads({4, 4, 1}, {4, 4, 1});
    commands->end_encoding();
    commands->begin_blit();
    commands->copy_texture_to_buffer(target, 0, 0, {0, 0, 0, 4, 4, 1}, readback, 0, 256, 4 * 256);
    commands->end_encoding();
    commands->commit();
    expect(commands->wait(UINT64_MAX), "bindless kernel command buffer completes");
    const auto written = pixel(readback->contents(), 256, 3, 3);
    const auto near = [](std::uint8_t value, int expected) { return std::abs(static_cast<int>(value) - expected) <= 1; };
    expect(near(written[0], 64) && near(written[1], 128) && near(written[2], 191) && written[3] == 255,
           "the kernel wrote the argument buffer's storage texture: " + format_pixel(written));
}

// Framebuffer fetch: a [[color(1)]] fragment input reads what an earlier
// draw of the same render pass wrote to color attachment 1 (input
// attachment at binding 224 + 1, dynamic rendering local read).
void test_framebuffer_fetch(const std::shared_ptr<metal::Device>& device) {
    if (!device->dynamic_rendering_local_read) {
        std::cout << "SKIP: framebuffer fetch (no VK_KHR_dynamic_rendering_local_read)\n";
        return;
    }
    const auto make = [&](msl::Function fragment, std::uint32_t mask1, const char* label) {
        metal::RenderPipelineDescriptor descriptor;
        descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
        descriptor.fragment = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(std::move(fragment)));
        descriptor.colors[0].pixel_format = descriptor.colors[1].pixel_format = metal::pixel_format::rgba8_unorm;
        descriptor.colors[1].write_mask = mask1;
        descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
        descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
        descriptor.label = label;
        return metal::RenderPipeline::create(device, descriptor);
    };
    auto writer = make(plain_fragment(fetch_write_frag_spirv, sizeof(fetch_write_frag_spirv), 2), 0xf, "fetch write");
    auto reader_function = plain_fragment(fetch_read_frag_spirv, sizeof(fetch_read_frag_spirv), 1);
    msl::ColorInput input;
    input.color = 1;
    input.name = "header";
    input.descriptor = {1, msl::color_input_binding_base + 1, msl::DescriptorType::input_attachment, 1};
    reader_function.reflection.color_inputs.push_back(input);
    auto reader = make(std::move(reader_function), 0, "fetch read");
    metal::RenderPass pass;
    for (int index = 0; index < 2; ++index) {
        pass.colors[index].texture = render_target(device);
        pass.colors[index].load_action = 2;
        pass.colors[index].store_action = 1;
        pass.colors[index].clear_color = {0, 0, 1, 1};
    }
    const auto readback = draw_top_half(device, pass, {writer, reader}, [](metal::CommandBuffer&, std::size_t) {});
    const auto inside = pixel(readback->contents(), 256, 32, 4), outside = pixel(readback->contents(), 256, 32, 60);
    const auto near = [](std::uint8_t value, int expected) { return std::abs(static_cast<int>(value) - expected) <= 1; };
    expect(near(inside[0], 191) && near(inside[1], 128) && near(inside[2], 64) && inside[3] == 255,
           "framebuffer fetch reads color(1) written by the previous draw (swizzled): " + format_pixel(inside));
    expect(outside == std::array<std::uint8_t, 4>{0, 0, 255, 255}, "clear color outside the fetch triangle: " + format_pixel(outside));
}

// An argument buffer of samplers ([[buffer(fc0 + 1)]], fc0 = 2): the
// MTLSamplerState tokens encoded into the buffer bound at index 3 fill the
// sampler array at binding 192; element 2 decides the result.
void test_argument_buffer_samplers(const std::shared_ptr<metal::Device>& device) {
    auto fragment = plain_fragment(argument_samplers_frag_spirv, sizeof(argument_samplers_frag_spirv), 1);
    msl::TextureBinding texture;
    texture.index = 0;
    texture.name = "image";
    texture.descriptor = {1, msl::texture_binding_base, msl::DescriptorType::sampled_image, 1};
    fragment.reflection.textures.push_back(texture);
    msl::ArgumentBufferSamplers samplers;
    samplers.index = 1;
    samplers.index_function_constant = 0;
    samplers.name = "samplers";
    samplers.type = "SStruct";
    samplers.members.push_back({"sampler_args", 0, 3});
    samplers.count = 3;
    samplers.descriptor = {1, msl::argument_buffer_sampler_binding_base, msl::DescriptorType::sampler, 3};
    fragment.reflection.argument_buffers.push_back(samplers);
    auto function = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(std::move(fragment)));
    const std::int32_t base = 2;
    function->constants[0].assign(reinterpret_cast<const std::uint8_t*>(&base), reinterpret_cast<const std::uint8_t*>(&base) + 4);
    expect(metal::argument_buffer_index(function->reflection().argument_buffers[0], &function->constants) == 3,
           "argument buffer index resolves fc0 + 1 with fc0 = 2");

    metal::RenderPipelineDescriptor descriptor;
    descriptor.vertex = std::make_shared<metal::ShaderFunction>(device, std::make_shared<msl::Function>(vertex_function()));
    descriptor.fragment = function;
    descriptor.colors[0].pixel_format = metal::pixel_format::rgba8_unorm;
    descriptor.attributes[0] = metal::VertexAttribute{29, 0, 0};
    descriptor.layouts[0] = metal::VertexLayout{8, 1, 1};
    descriptor.label = "argument buffer samplers";
    auto pipeline = metal::RenderPipeline::create(device, descriptor);

    metal::TextureDescriptor image_descriptor;
    image_descriptor.width = image_descriptor.height = 2;
    auto image = metal::Texture::create(device, image_descriptor);
    const std::uint8_t texels[] = {10, 20, 30, 255, 0, 255, 0, 255, 40, 50, 60, 255, 70, 80, 90, 255};
    image->replace_region({0, 0, 0, 2, 2, 1}, 0, 0, texels, 8, 0);
    metal::SamplerDescriptor repeat, zero;   // nearest
    repeat.address = {2, 2, 2};              // MTLSamplerAddressModeRepeat
    zero.address = {4, 4, 4};                // MTLSamplerAddressModeClampToZero
    auto other = metal::Sampler::create(device, zero);
    const auto run = [&](const std::shared_ptr<metal::Sampler>& chosen) {
        auto arguments = shared_buffer(device, 3 * 8);
        const std::uint64_t tokens[3] = {metal::sampler_token(other), 0, metal::sampler_token(chosen)};
        std::memcpy(arguments->contents(), tokens, sizeof(tokens));
        metal::RenderPass pass;
        pass.colors[0].texture = render_target(device);
        pass.colors[0].load_action = 2;
        pass.colors[0].store_action = 1;
        pass.colors[0].clear_color = {0, 0, 1, 1};
        const auto readback = draw_top_half(device, pass, {pipeline}, [&](metal::CommandBuffer& commands, std::size_t) {
            commands.set_texture(metal::Stage::fragment, 0, image);
            commands.set_buffer(metal::Stage::fragment, 3, arguments, 0);
        });
        return pixel(readback->contents(), 256, 32, 4);
    };
    const auto repeated = run(metal::Sampler::create(device, repeat));
    expect(repeated == std::array<std::uint8_t, 4>{0, 255, 0, 255},
           "argument buffer sampler 2 (repeat) wraps u = 1.75 to the green texel: " + format_pixel(repeated));
    const auto zeroed = run(metal::Sampler::create(device, zero));
    expect(zeroed == std::array<std::uint8_t, 4>{0, 0, 0, 0},
           "argument buffer sampler 2 (clamp to zero) gives transparent black: " + format_pixel(zeroed));
}
#endif

} // namespace

int main() {
    std::shared_ptr<metal::Device> device;
    try {
        metal::DeviceOptions options;
        options.pipeline_cache = std::filesystem::path{};   // no disk cache in tests
        options.presentation = false;
        device = metal::Device::create(options);
    } catch (const std::exception& error) {
        std::cout << "SKIP: no Vulkan 1.3 device for Metal: " << error.what() << '\n';
        return 77;
    }
    try {
        test_profile(device);
        test_buffers(device);
        test_host_buffers(device);
        test_textures(device);
        test_attachment_usage(device);
        test_vertex_signedness();
        test_events(device);
        test_timestamps(device);
        test_concurrent_completion(device);
        test_wrapped_layout(device);
#ifdef ALCHEMY_METAL_TEST_SHADERS
        test_compute(device);
        test_render(device, false);
        test_render(device, true);
        test_render(device, true, true);
        test_raster_variants(device);
        test_visibility_results(device, metal::StorageMode::private_);
        test_visibility_results(device, metal::StorageMode::shared);
        test_sampler_lod_clamp(device);
        test_framebuffer_fetch(device);
        test_depth_resolve_filters(device);
        test_deferred_resolve(device);
        test_instance_step_rate(device);
        test_dynamic_vertex_stride(device);
        test_vector_function_constants(device);
        test_texture_buffer_views_and_copies(device);
        test_placement_heap(device);
        test_linear_texture_over_buffer(device);
        test_tessellation(device);
        test_indirect_command_buffers(device);
        test_barycentric_coordinates(device);
        test_bindless_argument_buffers(device);
        test_argument_buffer_samplers(device);
#else
        std::cout << "SKIP: shader tests (glslangValidator was not found at configure time)\n";
#endif
    } catch (const std::exception& error) {
        std::cerr << "FAIL: exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " Metal-on-Vulkan check(s) failed\n";
        return 1;
    }
    std::cout << "Alchemy Metal-on-Vulkan tests passed\n";
    return 0;
}
