#pragma once

// Metal command buffers and encoders on Vulkan (owner: A5).
//
// A CommandBuffer records straight into Vulkan command buffers while the
// guest encodes. Encoders map as follows:
//   render  -> one vkCmdBeginRendering / vkCmdEndRendering instance (dynamic
//              rendering); pipeline, viewport, scissor, cull, winding, depth
//              bias, stencil reference, blend color and depth/stencil state
//              are dynamic state; vertex/fragment buffers, bytes, textures and
//              samplers are written into per-draw descriptor sets (argument
//              table of buffer device addresses at binding 0);
//   compute -> dispatches; serial encoders (the default) put a compute
//              barrier between dispatches, concurrent ones only on
//              memoryBarrier; the grid goes in push constants;
//   blit    -> transfer commands with a transfer barrier between commands.
// Every encoder starts with a full memory barrier, which gives Metal's
// default hazard tracking between encoders (and between command buffers).
// setBytes data, argument tables and visibility-query results live in
// per-command-buffer upload memory recycled when the GPU is done.
// encodeSignalEvent:/encodeWaitForEvent: split the recording into batches of
// one vkQueueSubmit2 with timeline semaphore signals/waits between them.
//
// Completion: commit() submits on the device queue (Device::submit) and
// schedules finalize() for when the timeline passes the submission's value:
// finalize recycles memory, writes visibility results and drops retained
// references. It runs on whichever thread observes completion first
// (status(), wait(), Device::collect()). Guest-visible handlers are the
// Objective-C layer's business.
//
// Threading: a CommandBuffer is encoded by one thread at a time (Metal's
// rule); status(), wait() and finalize are thread-safe.

#include "alchemy/darwin/metal/device.hpp"
#include "alchemy/darwin/metal/pipeline.hpp"
#include "alchemy/darwin/metal/resources.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace alchemy::metal {

// MTLEvent / MTLSharedEvent: a timeline semaphore.
class Event {
public:
    static std::shared_ptr<Event> create(std::shared_ptr<Device> device, bool shared);
    ~Event();
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    const std::shared_ptr<Device> device;
    VkSemaphore semaphore{};
    const bool shared;
    std::string label;
    [[nodiscard]] std::uint64_t signaled_value() const;
    // setSignaledValue: from the CPU. Vulkan timelines only increase: a value
    // at or below the current one is ignored.
    void signal(std::uint64_t value);
    // Waits for value; true when reached.
    bool wait(std::uint64_t value, std::uint64_t timeout_ns) const;
private:
    Event(std::shared_ptr<Device> owner, bool is_shared) : device(std::move(owner)), shared(is_shared) {}
};

// MTLCounterSampleBuffer of the timestamp counter set: one Vulkan timestamp
// query per sample, in GPU ticks (DeviceProfile::timestamp_period ns each).
class CounterSampleBuffer {
public:
    static constexpr std::uint64_t error_value = ~std::uint64_t{0};   // MTLCounterErrorValue
    static std::shared_ptr<CounterSampleBuffer> create(std::shared_ptr<Device> device, std::uint32_t count);
    ~CounterSampleBuffer();
    CounterSampleBuffer(const CounterSampleBuffer&) = delete;
    CounterSampleBuffer& operator=(const CounterSampleBuffer&) = delete;
    const std::shared_ptr<Device> device;
    const std::uint32_t count;
    VkQueryPool pool{};
    // -resolveCounterRange: from the CPU: each sample's value once the GPU
    // wrote it, error_value for one never written (or reset for rewriting).
    [[nodiscard]] std::vector<std::uint64_t> resolve(std::uint32_t first, std::uint32_t length) const;
    // Samples some recorded command writes (resolveCounters: copies only
    // those; drivers differ in what they copy for unavailable queries).
    void mark_recorded(std::uint32_t index);
    [[nodiscard]] std::vector<bool> recorded(std::uint32_t first, std::uint32_t length) const;
private:
    CounterSampleBuffer(std::shared_ptr<Device> owner, std::uint32_t samples)
        : device(std::move(owner)), count(samples), recorded_(samples) {}
    mutable std::mutex mutex_;
    std::vector<bool> recorded_;
};
// The samples an encoder takes at its start and end
// (MTL*PassSampleBufferAttachmentDescriptor). A render pass has no
// timestamps inside it here: its vertex and fragment start samples are taken
// when the pass begins and its end samples when it ends.
struct CounterSample { std::shared_ptr<CounterSampleBuffer> buffer; std::uint32_t index{}; };
struct EncoderSamples { std::vector<CounterSample> start, end; };

enum class CommandBufferStatus : std::uint32_t { not_enqueued = 0, enqueued = 1, committed = 2, scheduled = 3,
    completed = 4, error = 5 };

struct Attachment {
    std::shared_ptr<Texture> texture;
    std::uint32_t level{}, slice{}, depth_plane{};
    std::uint32_t load_action{};      // MTLLoadAction
    std::uint32_t store_action{};     // MTLStoreAction
    std::shared_ptr<Texture> resolve_texture;
    std::uint32_t resolve_level{}, resolve_slice{}, resolve_depth_plane{};
    std::array<double, 4> clear_color{0, 0, 0, 1};
    double clear_depth{1};
    std::uint32_t clear_stencil{};
};
struct RenderPass {
    std::array<Attachment, 8> colors{};
    Attachment depth, stencil;
    std::uint32_t width{}, height{}, layers{};   // renderTargetWidth/Height/ArrayLength (used without attachments)
    std::uint32_t default_sample_count{1};
    std::shared_ptr<Buffer> visibility;
    EncoderSamples samples;
};

// MTLViewport and MTLScissorRect.
struct Viewport { double x{}, y{}, width{}, height{}, znear{}, zfar{1}; };
struct Scissor { std::uint64_t x{}, y{}, width{}, height{}; };

enum class Stage : std::uint32_t { vertex = 0, fragment = 1, kernel = 2 };
enum class EncoderKind : std::uint32_t { none, render, compute, blit };

class CommandBuffer : public std::enable_shared_from_this<CommandBuffer> {
public:
    static std::shared_ptr<CommandBuffer> create(std::shared_ptr<Device> device, bool retained_references = true);
    ~CommandBuffer();
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    const std::shared_ptr<Device> device;
    const bool retained_references;
    std::string label;

    // ---- encoders --------------------------------------------------------------------
    [[nodiscard]] EncoderKind encoder() const noexcept { return encoder_; }
    void begin_render(const RenderPass& pass);
    void begin_compute(bool concurrent, EncoderSamples samples = {});
    void begin_blit(EncoderSamples samples = {});
    void end_encoding();
    void push_debug_group(const std::string& name);
    void pop_debug_group();
    void insert_debug_signpost(const std::string& name);

    // Bindings (render: vertex/fragment; compute: kernel).
    void set_buffer(Stage stage, std::uint32_t index, std::shared_ptr<Buffer> buffer, std::uint64_t offset);
    void set_buffer_offset(Stage stage, std::uint32_t index, std::uint64_t offset);
    void set_bytes(Stage stage, std::uint32_t index, std::span<const std::uint8_t> bytes);
    void set_texture(Stage stage, std::uint32_t index, std::shared_ptr<Texture> texture);
    void set_sampler(Stage stage, std::uint32_t index, std::shared_ptr<Sampler> sampler);

    // Render state.
    void set_render_pipeline(std::shared_ptr<RenderPipeline> pipeline);
    void set_viewports(std::span<const Viewport> viewports);
    void set_scissors(std::span<const Scissor> scissors);
    void set_cull_mode(std::uint32_t mode);           // MTLCullMode
    void set_front_facing(std::uint32_t winding);     // MTLWinding
    void set_depth_bias(float bias, float slope, float clamp);
    void set_stencil_reference(std::uint32_t front, std::uint32_t back);
    void set_blend_color(float red, float green, float blue, float alpha);
    void set_depth_stencil(std::shared_ptr<DepthStencilState> state);
    void set_visibility_mode(std::uint32_t mode, std::uint64_t offset);   // MTLVisibilityResultMode
    void draw(std::uint32_t primitive, std::uint32_t vertex_start, std::uint32_t vertex_count,
              std::uint32_t instance_count, std::uint32_t base_instance);
    void draw_indexed(std::uint32_t primitive, std::uint32_t index_count, std::uint32_t index_type,
                      const std::shared_ptr<Buffer>& index_buffer, std::uint64_t index_offset,
                      std::uint32_t instance_count, std::int32_t base_vertex, std::uint32_t base_instance);
    void draw_indirect(std::uint32_t primitive, const std::shared_ptr<Buffer>& indirect, std::uint64_t offset);
    void draw_indexed_indirect(std::uint32_t primitive, std::uint32_t index_type, const std::shared_ptr<Buffer>& index_buffer,
                               std::uint64_t index_offset, const std::shared_ptr<Buffer>& indirect, std::uint64_t offset);
    // memoryBarrierWithScope: (MTLBarrierScope bits) in render and compute encoders.
    void memory_barrier(std::uint64_t scope);

    // Compute state.
    void set_compute_pipeline(std::shared_ptr<ComputePipeline> pipeline);
    void set_threadgroup_memory(std::uint32_t index, std::uint32_t length);
    void dispatch_threadgroups(const std::array<std::uint32_t, 3>& groups, const std::array<std::uint32_t, 3>& threads);
    void dispatch_threads(const std::array<std::uint32_t, 3>& grid, const std::array<std::uint32_t, 3>& threads);
    void dispatch_indirect(const std::shared_ptr<Buffer>& indirect, std::uint64_t offset,
                           const std::array<std::uint32_t, 3>& threads);

    // Blit commands.
    void copy_buffer(const std::shared_ptr<Buffer>& source, std::uint64_t source_offset,
                     const std::shared_ptr<Buffer>& destination, std::uint64_t destination_offset, std::uint64_t size);
    void copy_buffer_to_texture(const std::shared_ptr<Buffer>& source, std::uint64_t offset, std::uint64_t bytes_per_row,
        std::uint64_t bytes_per_image, const Region& size, const std::shared_ptr<Texture>& destination,
        std::uint32_t slice, std::uint32_t level, std::uint64_t options = 0);
    void copy_texture_to_buffer(const std::shared_ptr<Texture>& source, std::uint32_t slice, std::uint32_t level,
        const Region& region, const std::shared_ptr<Buffer>& destination, std::uint64_t offset,
        std::uint64_t bytes_per_row, std::uint64_t bytes_per_image, std::uint64_t options = 0);
    void copy_texture(const std::shared_ptr<Texture>& source, std::uint32_t source_slice, std::uint32_t source_level,
        const Region& region, const std::shared_ptr<Texture>& destination, std::uint32_t destination_slice,
        std::uint32_t destination_level, std::uint32_t x, std::uint32_t y, std::uint32_t z);
    void copy_texture_all(const std::shared_ptr<Texture>& source, const std::shared_ptr<Texture>& destination);
    void fill_buffer(const std::shared_ptr<Buffer>& buffer, std::uint64_t offset, std::uint64_t length, std::uint8_t value);
    void generate_mipmaps(const std::shared_ptr<Texture>& texture);
    // resolveCounters:inRange:destinationBuffer:destinationOffset: (8 bytes
    // per sample; error_value for samples not written when it runs).
    void resolve_counters(const std::shared_ptr<CounterSampleBuffer>& samples, std::uint32_t first, std::uint32_t length,
                          const std::shared_ptr<Buffer>& destination, std::uint64_t offset);

    // ---- events -----------------------------------------------------------------------------
    // (Drawables are presented by message after commit: interop.hpp.)
    void encode_signal(const std::shared_ptr<Event>& event, std::uint64_t value);
    void encode_wait(const std::shared_ptr<Event>& event, std::uint64_t value);

    // ---- lifecycle ----------------------------------------------------------------------
    void enqueue();
    // Ends an open encoder (Metal requires endEncoding; committing with one
    // open is a guest error Metal asserts on, here it is closed) and submits.
    // Throws VulkanError on submission failure (status error).
    void commit();
    [[nodiscard]] CommandBufferStatus status();
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    // waitUntilCompleted: true once completed (or failed); `timeout_ns`
    // bounds one wait (callers loop to poll for process stops).
    bool wait(std::uint64_t timeout_ns);
    [[nodiscard]] std::uint64_t timeline_value() const noexcept { return value_; }
    // GPU start/end times are not measured: kernelStartTime etc. report 0.
    // Runs host work once the GPU finished (before guest handlers).
    void on_complete(std::function<void()> work);
    // Retains an object until completion (retainedReferences).
    void retain(std::shared_ptr<void> object);

private:
    struct Binding {
        std::shared_ptr<Buffer> buffer;
        VkDeviceAddress address{};    // buffer address + offset (or upload address for bytes)
        std::uint64_t offset{};
        VkBuffer vk_buffer{};         // for vertex input
        VkDeviceSize vk_offset{};
    };
    struct StageState {
        std::array<Binding, 31> buffers{};
        std::vector<std::shared_ptr<Texture>> textures = std::vector<std::shared_ptr<Texture>>(128);
        std::array<std::shared_ptr<Sampler>, 16> samplers{};
        bool dirty{true};
        VkDescriptorSet set{};
    };
    struct Upload { VkBuffer buffer{}; VkDeviceAddress address{}; VkDeviceSize offset{}; std::uint8_t* pointer{}; };

    explicit CommandBuffer(std::shared_ptr<Device> owner, bool retained);
    VkCommandBuffer recording();
    void close_recording();
    void barrier(VkPipelineStageFlags2 source, VkAccessFlags2 source_access, VkPipelineStageFlags2 destination,
                 VkAccessFlags2 destination_access);
    void full_barrier();
    // External images (drawables, interop.hpp) are moved to GENERAL at their
    // first use in this command buffer: inline before blit/compute commands
    // and render passes (prepare_texture), or at the start of the command
    // buffer for a texture bound inside a render pass (prepare_in_pass), where
    // Vulkan allows no layout transition.
    // `write`: the use may write the texture (g3: IOSurface-backed textures
    // copy back into the surface only after such uses, note_surface).
    void prepare_texture(const std::shared_ptr<Texture>& texture, bool write = true);
    void prepare_in_pass(const std::shared_ptr<Texture>& texture, bool write = true);
    // g3: IOSurface-backed textures (resources.hpp, SurfaceBacking): the
    // batches that use each (commit() records their heads there) and those
    // written since the last tail (flush_surface_tails, before an event
    // signal and at commit).
    void note_surface(const Texture& texture, bool write);
    void flush_surface_tails(VkCommandBuffer command);
    struct SurfaceUse { std::shared_ptr<SurfaceBacking> backing; std::size_t batch{}; std::uint64_t writes_before{}; };
    std::vector<SurfaceUse> surface_uses_;
    std::vector<std::shared_ptr<SurfaceBacking>> surface_writes_;
    // Claims the external image for this command buffer: true the first time
    // (the caller transitions it from `layout`), false once it is GENERAL here.
    bool claim_external(const Texture& texture, VkImageLayout& layout);
    Upload upload(VkDeviceSize size, VkDeviceSize alignment);
    VkDescriptorSet allocate_set(VkDescriptorSetLayout layout);
    VkDescriptorSet write_set(StageState& state, const StageLayout& layout, const msl::Reflection* reflection);
    void flush_render(std::uint32_t primitive);
    void flush_compute(const std::array<std::uint32_t, 3>& threads, const std::array<std::uint32_t, 3>& grid, bool non_uniform);
    void end_render();
    void require(EncoderKind kind, const char* operation) const;
    // Resets and writes the samples' queries (outside a rendering instance).
    void write_samples(const std::vector<CounterSample>& samples, VkPipelineStageFlags2 stage);
    std::vector<CounterSample> end_samples_;   // of the open encoder
    void finalize();
    void release_resources();
    StageState& stage_state(Stage stage);

    EncoderKind encoder_{EncoderKind::none};
    CommandContext commands_;
    VkCommandBuffer current_{};
    std::vector<Device::Batch> batches_;
    std::vector<VkDescriptorPool> descriptor_pools_;
    struct Chunk { VkBuffer buffer{}; Allocation memory; VkDeviceAddress address{}; VkDeviceSize size{}; };
    std::vector<Chunk> chunks_;   // upload memory, the last one is current
    VkDeviceSize chunk_offset_{};
    std::vector<std::shared_ptr<void>> retained_;
    std::vector<std::function<void()>> completions_;
    std::vector<std::shared_ptr<Event>> events_;
    std::vector<VkImage> external_images_;              // external images already GENERAL in this command buffer
    std::vector<VkImageMemoryBarrier2> head_barriers_;  // recorded ahead of everything at commit

    // Render encoder state.
    RenderPass pass_;
    VkRenderingInfo rendering_{};
    std::vector<VkRenderingAttachmentInfo> color_infos_;
    // Vulkan's *_WITH_COUNT viewport and scissor counts must agree; Metal
    // lets them differ (missing scissors cover the render target).
    std::uint32_t viewport_count_{1};
    std::vector<VkRect2D> scissors_;
    void emit_scissors();
    VkRenderingAttachmentInfo depth_info_{}, stencil_info_{};
    VkExtent2D extent_{};
    std::uint32_t pass_layers_{1};
    std::shared_ptr<RenderPipeline> render_pipeline_;
    VkPipeline bound_pipeline_{};
    VkPipelineLayout bound_layout_{};
    std::shared_ptr<DepthStencilState> depth_stencil_;
    std::array<StageState, 2> render_stages_{};
    std::uint32_t bound_topology_{UINT32_MAX};
    std::array<VkBuffer, 31> bound_vertex_buffers_{};
    std::array<VkDeviceSize, 31> bound_vertex_offsets_{};
    // Visibility results.
    VkQueryPool queries_{};
    std::uint32_t query_count_{}, query_capacity_{};
    bool query_active_{};
    std::vector<std::uint64_t> query_offsets_;
    // ALCHEMY_METAL_TRACE_INDIRECT=1: the arguments of each indirect draw of
    // the pass are copied out after it and printed on completion.
    struct IndirectTrace { VkBuffer buffer{}; VkDeviceSize offset{}, size{}; std::string pipeline; };
    std::vector<IndirectTrace> indirect_traces_;

    // Compute encoder state.
    bool concurrent_{};
    bool dispatched_{};
    std::shared_ptr<ComputePipeline> compute_pipeline_;
    StageState compute_stage_;
    std::vector<std::uint32_t> threadgroup_memory_;
    bool blit_started_{};

    std::mutex status_mutex_;
    std::condition_variable status_changed_;
    CommandBufferStatus status_{CommandBufferStatus::not_enqueued};
    std::string error_;
    std::uint64_t value_{};
    bool finalized_{};
    bool final_status_{};                 // finalize() finished: status_ is completed or error
    std::thread::id finalizer_;           // the thread running finalize()
};

} // namespace alchemy::metal
