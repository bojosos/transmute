// Metal-on-Vulkan headless tests (owner: A5): the host layer below the
// Objective-C providers, on the machine's Vulkan 1.3 GPU. Shaders are GLSL
// written to the MSL contract's fixed resource model and compiled to SPIR-V
// at build time; their Reflection is written by hand exactly as the MSL
// compiler reports it. Exit code 77 (ctest SKIP) when no device exists.
#include "alchemy/darwin/metal/commands.hpp"
#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/formats.hpp"
#include "alchemy/darwin/metal/pipeline.hpp"
#include "alchemy/darwin/metal/resources.hpp"

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

// setSamplerState:lodMinClamp:lodMaxClamp:atIndex: (ANGLE's per-texture
// base/max level): the binding's sampler gets the given LOD range. A
// two-level texture magnified over the triangle samples level 0 unless the
// clamp's minimum forces level 1.
std::shared_ptr<metal::Texture> render_target(const std::shared_ptr<metal::Device>& device);
std::string format_pixel(const std::array<std::uint8_t, 4>& p);
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
        test_sampler_lod_clamp(device);
        test_framebuffer_fetch(device);
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
