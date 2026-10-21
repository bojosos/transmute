#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/darwin/quartzcore/quartzcore_vulkan.hpp"
#include "alchemy/gui/runloop.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/mach_kernel_objects.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <thread>

// IOSurface.framework over guest memory. A surface is a CF-type object of the
// host class IOSurface (the IOSurfaceRef is the bridged Objective-C object, so
// CFRetain/CFRelease, objc_retain and -[CALayer setContents:] all accept it).
// Its pixels are one block of guest memory that stays at the same address for
// the surface's lifetime and as long as GPU work on it may run: vm_allocate'd
// pages (fx shared memory), or private guest memory where the engine has none.
// IOSurface-backed MTLTextures (Metal's surface_textures.cpp, through
// surface_pages in quartzcore_vulkan.hpp) render into and sample from these
// pages. Like macOS's implicit GPU synchronization of IOSurfaces,
// IOSurfaceLock (without kIOSurfaceLockAvoidSync) and the compositor wait for
// the GPU writes submitted so far in any process of the tree; every completed
// GPU write and every IOSurfaceUnlock after writing advances the seed, which
// the tree shares like the use count. Only single-plane
// (chunky) surfaces are created; planar property sets fail explicitly. Lookup
// by IOSurfaceID works inside the process. IOSurfaceCreateMachPort makes a
// send right to a kernel-held port that keeps the surface alive
// (alchemy/mach_kernel_objects.hpp), and IOSurfaceLookupFromMachPort finds the
// surface again: Chrome's GPU process hands its software compositor's frames
// to the browser's CALayer tree that way. A port sent to another process
// carries the surface's description and shares its pixel pages; there the
// lookup answers a local IOSurface over the same pages (one per remote
// surface, reused while it lives), so stores in either process are seen in
// both. IOSurfaceIDs are unique across the process tree and a surface keeps
// its ID and its use count in every process (IOSurfaceIncrementUseCount,
// DecrementUseCount, GetUseCount, IsInUse; the compositor holds a use while
// the surface is shown, like the window server). Values (IOSurfaceSetValue)
// stay per process.
namespace alchemy::gui::support_kit {
namespace {

constexpr std::uint32_t kern_success = 0, kern_invalid_argument = 4;
// IOSurfaceLockOptions (IOSurfaceTypes.h).
constexpr std::uint32_t lock_read_only = 1, lock_avoid_sync = 2;
// How long IOSurfaceLock and the compositor wait for pending GPU writes (a
// GPU process that died leaves its writes pending forever).
constexpr auto gpu_write_wait = std::chrono::seconds(2);
// Rows align to 64 bytes; allocations to the arm64 page (16 KiB), so a
// surface's pages can be shared whole.
constexpr std::uint64_t row_alignment = 64, size_alignment = 16384;
constexpr std::uint64_t maximum_dimension = 32768;

// The process tree a section name is scoped to: the tree's Mach broker.
std::string tree_name() {
    return host::process::environment("ALCHEMY_MACH_BROKER").value_or(std::to_string(host::process::current_pid()));
}

// One surface's state that Darwin keeps system-wide, shared by every process
// of the tree that has the surface (a 4 KiB section named after the tree and
// the IOSurfaceID, zero-filled):
//   +0  int32  use count: IOSurfaceIsInUse in the producer (viz's output
//              device) sees the browser's use of a displayed surface;
//   +4  uint32 seed;
//   +8  uint64 GPU writes submitted (tickets handed out by begin_gpu_write);
//   +16 uint64 GPU writes completed (the highest finished ticket).
// Without host sections it is this process's state. Holders: the surface
// itself, every SurfaceUse taken on it and every SurfacePages handed to Metal
// (they outlive the surface object).
struct SharedState {
    host::Handle handle{};
    std::uintptr_t view{};
    struct Layout {
        std::atomic<std::int32_t> uses{0};
        std::atomic<std::uint32_t> seed{0};
        std::atomic<std::uint64_t> gpu_writes{0}, gpu_written{0};
    };
    Layout local;
    Layout* value{&local};
    SharedState() = default;
    SharedState(const SharedState&) = delete;
    SharedState& operator=(const SharedState&) = delete;
    ~SharedState() {
        if (view) static_cast<void>(host::sections::unmap(view));
        if (handle) host::close(handle);
    }
    void increment() { value->uses.fetch_add(1); }
    void decrement() {   // never below 0
        auto count = value->uses.load();
        while (count > 0 && !value->uses.compare_exchange_weak(count, count - 1)) {}
    }
    [[nodiscard]] std::int32_t count() const { return value->uses.load(); }
    [[nodiscard]] std::uint32_t seed() const { return value->seed.load(); }
    // Waits until the GPU writes submitted before the call (or up to
    // `ticket`) have completed; false after `timeout`.
    bool wait_for_gpu(std::chrono::steady_clock::duration timeout, std::uint64_t ticket = UINT64_MAX) const {
        const auto target = std::min(ticket, value->gpu_writes.load());
        if (value->gpu_written.load() >= target) return true;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto pause = std::chrono::microseconds(50);
        while (value->gpu_written.load() < target) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(pause);
            pause = std::min(pause * 2, std::chrono::microseconds(1000));
        }
        return true;
    }
};
static_assert(sizeof(SharedState::Layout) == 24 && offsetof(SharedState::Layout, gpu_writes) == 8);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<std::int32_t>::is_always_lock_free);
std::shared_ptr<SharedState> shared_state_for(std::uint32_t id) {
    auto state = std::make_shared<SharedState>();
    if (const auto named = host::sections::open_named("Alchemy.IOSurfaceUse." + tree_name() + "." + std::to_string(id), 4096)) {
        if (const auto view = host::sections::map(named->handle, 0, 4096, true)) {
            state->handle = named->handle;
            state->view = *view;
            state->value = reinterpret_cast<SharedState::Layout*>(*view);   // zero-filled
        } else {
            host::close(named->handle);
        }
    }
    return state;
}

// A surface's pixel pages. The surface holds them, and so does every
// SurfacePages handed to Metal, whose GPU work may still read or write them
// after the surface object is gone. The last holder other than the surface's
// own -dealloc may be any thread: it queues the pages for vm_deallocate on
// the main run loop (Releases below).
struct Pages {
    Address base{};
    std::uint64_t size{};
    bool vm{};
    bool released{};   // freed by the surface's -dealloc
    ~Pages();
};

// Waits for pending GPU writes (IOSurfaceLock, the compositor); reports once
// when the wait times out.
void wait_for_gpu_writes(const SharedState& state, const char* who) {
    if (state.wait_for_gpu(gpu_write_wait)) return;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true))
        std::fprintf(stderr, "[alchemy iosurface] %s: GPU writes still pending after %lld ms; reading the pages as they are\n", who,
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(gpu_write_wait).count()));
}

// One plane of a planar surface (kIOSurfacePlaneInfo): YCbCr video frames
// such as '420v' (NV12), a luma plane and an interleaved chroma plane.
struct Plane {
    std::uint32_t width{}, height{}, bytes_per_element{1}, element_width{1}, element_height{1};
    std::uint64_t bytes_per_row{}, offset{}, size{};
};
constexpr std::size_t max_planes = 4;

struct Surface final : ObjectPayload {
    std::uint32_t id{};
    Address base{};
    std::uint32_t width{}, height{}, bytes_per_element{4}, element_width{1}, element_height{1};
    std::uint64_t bytes_per_row{}, alloc_size{};
    std::uint32_t pixel_format{};
    std::mutex mutex;
    std::shared_ptr<SharedState> uses;       // set with the IOSurfaceID: use count, seed, GPU writes
    std::shared_ptr<Pages> pages;            // set with the IOSurfaceID; cleared by -dealloc
    std::atomic<std::int32_t> locks{0};
    std::uint32_t purgeable{};               // IOSurfacePurgeabilityState (under mutex)
    std::map<std::string, Address> values;   // retained CF values
    bool vm{};                               // the pixels are vm_allocate'd (or mapped) pages
    std::vector<Plane> planes;               // planar surfaces only; the top-level fields describe plane 0
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint32_t, Address> by_id;   // weak: removed in destroy
    std::uint32_t next{1};                    // without the shared counter
};
Registry& registry() { static Registry value; return value; }

// IOSurfaceIDs are unique across the processes of one Alchemy process tree,
// like Darwin's system-wide IDs: a counter in a section shared by name (the
// tree's Mach broker names it), so a surface keeps its ID in every process it
// reaches. Without the shared section (no host sections), a process counter.
std::uint32_t next_surface_id() {
    struct Counter {
        std::mutex mutex;
        bool opened{};
        host::Handle handle{};
        std::atomic<std::uint32_t>* value{};
    };
    static Counter counter;
    {
        std::lock_guard lock(counter.mutex);
        if (!counter.opened) {
            counter.opened = true;
            if (const auto named = host::sections::open_named("Alchemy.IOSurfaceIDs." + tree_name(), 4096)) {
                if (const auto view = host::sections::map(named->handle, 0, 4096, true)) {
                    counter.handle = named->handle;
                    counter.value = reinterpret_cast<std::atomic<std::uint32_t>*>(*view);   // zero-filled, lock-free
                }
            }
        }
    }
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    if (counter.value) {
        for (;;) {
            const auto id = counter.value->fetch_add(1) + 1;
            if (id) return id;   // 0 is no IOSurfaceID
        }
    }
    auto& table = registry();
    std::lock_guard lock(table.mutex);
    return table.next++;
}

void release_pixels(Support& support, Address base, std::uint64_t size, bool vm);

// Work that needs a guest thread, done on the main run loop: the release of
// an IOSurface Mach port's surface (its destructor runs with the IPC space
// locked) and pixel pages whose last holder was host code (Pages).
struct Releases {
    struct PagesToFree { Address base{}; std::uint64_t size{}; bool vm{}; };
    std::mutex mutex;
    std::vector<Address> pending;
    std::vector<PagesToFree> pages;
    SourceId source{};
    std::weak_ptr<Support> owner;
};
Releases& releases() { static Releases value; return value; }
void drain_releases() {
    auto& r = releases();
    std::vector<Address> objects;
    std::vector<Releases::PagesToFree> pages;
    {
        std::lock_guard lock(r.mutex);
        objects.swap(r.pending);
        pages.swap(r.pages);
    }
    const auto owner = r.owner.lock();
    if (!owner) return;
    for (const auto object : objects) owner->release(object);
    for (const auto& free : pages) release_pixels(*owner, free.base, free.size, free.vm);
}
void signal_releases(SourceId source) {
    try {
        if (source && main_run_loop_available()) main_run_loop().signal_source(source);
    } catch (...) {}   // process teardown: the memory goes with the process
}
Pages::~Pages() {
    if (released || !base) return;
    auto& r = releases();
    SourceId source{};
    {
        std::lock_guard lock(r.mutex);
        r.pages.push_back({base, size, vm});
        source = r.source;
    }
    signal_releases(source);
}
struct SurfacePort final : MachKernelObject {
    Address surface{};
    std::shared_ptr<Surface> state;
    // Another process: the description (version 2) and the pixel pages.
    [[nodiscard]] std::optional<Wire> wire() const override {
        if (!state || !state->vm) return std::nullopt;
        Wire out{"IOSurface", {}, state->base, state->alloc_size};
        const auto put = [&](const auto value) {
            const auto at = out.data.size();
            out.data.resize(at + sizeof value);
            std::memcpy(out.data.data() + at, &value, sizeof value);
        };
        put(std::uint32_t{3});
        put(state->id);
        put(state->width); put(state->height); put(state->bytes_per_element);
        put(state->element_width); put(state->element_height); put(state->pixel_format);
        put(state->bytes_per_row); put(state->alloc_size);
        // Version 3: the planes.
        put(static_cast<std::uint32_t>(state->planes.size()));
        for (const auto& plane : state->planes) {
            put(plane.width); put(plane.height); put(plane.bytes_per_element); put(plane.element_width);
            put(plane.element_height); put(plane.bytes_per_row); put(plane.offset); put(plane.size);
        }
        return out;
    }
    ~SurfacePort() override {
        auto& r = releases();
        SourceId source{};
        {
            std::lock_guard lock(r.mutex);
            r.pending.push_back(surface);
            source = r.source;
        }
        signal_releases(source);
    }
};

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

std::shared_ptr<Surface> find_surface(Support& support, Address object) {
    return object ? support.objc.find_host_state<Surface>(object) : nullptr;
}
std::shared_ptr<Surface> require_surface(Support& support, Address object, std::string_view function) {
    auto surface = find_surface(support, object);
    if (!surface) fail(std::string(function) + ": the argument is not an IOSurfaceRef");
    return surface;
}

std::uint64_t task_self(Support& support) {
    const auto found = support.process.resolve(libsystem_library, "_mach_task_self_");
    return found ? support.memory().read32(found->address) : 0;
}
// Pixel memory: vm_allocate'd pages (zero-filled; another process can map
// them), else private guest memory.
Address allocate_pixels(Support& support, std::uint64_t size, bool& vm) {
    const auto task = task_self(support);
    if (task && support.process.resolve(libsystem_library, "_vm_allocate")) {
        const auto out = support.process.allocate_guest(8, 8);
        support.memory().write64(out, 0);
        const auto result = support.call(libsystem_library, "_vm_allocate", {task, out, size, 1 /* VM_FLAGS_ANYWHERE */});
        const auto address = support.memory().read64(out);
        support.process.release_guest(out);
        if (static_cast<std::uint32_t>(result) == 0 && address) {
            vm = true;
            return address;
        }
    }
    vm = false;
    const auto address = support.process.allocate_guest(size, size_alignment);
    support.memory().fill_bytes(address, 0, size);
    return address;
}
void release_pixels(Support& support, Address base, std::uint64_t size, bool vm) {
    if (!vm) {
        support.process.release_guest(base);
        return;
    }
    if (const auto task = task_self(support)) static_cast<void>(support.call(libsystem_library, "_vm_deallocate", {task, base, size}));
}

// The IOSurface object for `surface` (+1), registered under its IOSurfaceID
// (a new one unless it came from another process).
Address make_surface_object(Support& support, Address cls, const std::shared_ptr<Surface>& surface) {
    if (!surface->id) surface->id = next_surface_id();
    if (!surface->uses) surface->uses = shared_state_for(surface->id);
    if (!surface->pages) {
        surface->pages = std::make_shared<Pages>();
        surface->pages->base = surface->base;
        surface->pages->size = surface->alloc_size;
        surface->pages->vm = surface->vm;
    }
    const auto id = surface->id;
    std::weak_ptr<Support> owner = support.shared_from_this();
    const auto object = support.create(cls, surface, [owner, id](Address self) {
        {
            auto& table = registry();
            std::lock_guard lock(table.mutex);
            if (const auto found = table.by_id.find(id); found != table.by_id.end() && found->second == self) table.by_id.erase(found);
        }
        const auto keeper = owner.lock();
        if (!keeper) return;
        const auto state = keeper->objc.find_host_state<Surface>(self);
        if (!state) return;
        std::map<std::string, Address> values;
        std::shared_ptr<Pages> pages;
        {
            std::lock_guard lock(state->mutex);
            values.swap(state->values);
            pages.swap(state->pages);
        }
        for (const auto& [key, value] : values) keeper->release(value);
        // Freed here unless GPU work (an IOSurface-backed texture) still
        // holds the pages; then their last holder queues the release.
        if (pages && pages.use_count() == 1) {
            pages->released = true;
            release_pixels(*keeper, pages->base, pages->size, pages->vm);
        }
    });
    {
        auto& table = registry();
        std::lock_guard lock(table.mutex);
        table.by_id[id] = object;
    }
    return object;
}

// The receiver of "IOSurface" Mach port objects from another process: the
// surface with that IOSurfaceID if one lives here (the creator's own, or one
// that arrived before: Chrome cycles a small pool), else a local surface over
// the mapped pages [address, address + size) under the same ID. Owns the
// mapping: released when an existing surface is answered.
std::shared_ptr<MachKernelObject> receive_surface(Support& support, Address cls, const std::vector<std::uint8_t>& data,
                                                  Address address, std::uint64_t size) {
    std::size_t at = 0;
    bool ok = true;
    const auto get = [&](auto& value) {
        if (data.size() - at < sizeof value) { ok = false; return; }
        std::memcpy(&value, data.data() + at, sizeof value);
        at += sizeof value;
    };
    std::uint32_t version{};
    auto surface = std::make_shared<Surface>();
    get(version);
    get(surface->id);
    get(surface->width); get(surface->height); get(surface->bytes_per_element);
    get(surface->element_width); get(surface->element_height); get(surface->pixel_format);
    get(surface->bytes_per_row); get(surface->alloc_size);
    std::uint32_t planes{};
    if (version == 3) get(planes);
    for (std::uint32_t index = 0; ok && index < planes && index < max_planes; ++index) {
        Plane plane;
        get(plane.width); get(plane.height); get(plane.bytes_per_element); get(plane.element_width);
        get(plane.element_height); get(plane.bytes_per_row); get(plane.offset); get(plane.size);
        if (plane.offset > size || plane.size > size - plane.offset) ok = false;
        surface->planes.push_back(plane);
    }
    if (!ok || (version != 2 && version != 3) || planes > max_planes || !surface->id || !address || size < surface->alloc_size ||
        !surface->width || !surface->height || surface->bytes_per_row * surface->height > size) {
        if (address) release_pixels(support, address, size, true);
        return nullptr;
    }
    auto port = std::make_shared<SurfacePort>();
    Address existing{};
    {
        auto& table = registry();
        std::lock_guard lock(table.mutex);
        if (const auto found = table.by_id.find(surface->id); found != table.by_id.end()) existing = found->second;
    }
    if (existing && support.process.objects().try_retain(existing)) {
        release_pixels(support, address, size, true);   // its pages are mapped here already
        port->surface = existing;
        port->state = find_surface(support, existing);
        return port;
    }
    surface->base = address;
    surface->alloc_size = size;
    surface->vm = true;
    port->state = surface;
    port->surface = make_surface_object(support, cls, surface);
    return port;
}

std::optional<std::uint64_t> number_property(Support& support, Address dictionary, std::string_view key) {
    for (const auto& [name, value] : support.cf.dictionary_entries(dictionary)) {
        if (support.cf.utf8(name) != key) continue;
        const auto number = support.cf.number_value(value);
        if (!number || *number < 0) fail("IOSurfaceCreate: property " + std::string(key) + " is not a non-negative number");
        return static_cast<std::uint64_t>(*number);
    }
    return std::nullopt;
}

// kIOSurfacePlaneInfo: one dictionary per plane. Each plane's rows fit its
// bytes per row and its pixels fit [offset, offset + size); the surface's
// allocation covers every plane. Empty without the key; nullopt for
// impossible planes, for which IOSurfaceCreate answers NULL.
std::optional<std::vector<Plane>> plane_properties(Support& support, Address properties) {
    std::vector<Plane> planes;
    for (const auto& [name, value] : support.cf.dictionary_entries(properties)) {
        if (support.cf.utf8(name) != "IOSurfacePlaneInfo") continue;
        const auto entries = support.cf.array_values(value);
        if (entries.empty() || entries.size() > max_planes) return std::nullopt;
        for (const auto entry : entries) {
            Plane plane;
            plane.width = static_cast<std::uint32_t>(number_property(support, entry, "IOSurfacePlaneWidth").value_or(0));
            plane.height = static_cast<std::uint32_t>(number_property(support, entry, "IOSurfacePlaneHeight").value_or(0));
            plane.bytes_per_element =
                static_cast<std::uint32_t>(number_property(support, entry, "IOSurfacePlaneBytesPerElement").value_or(1));
            plane.element_width = static_cast<std::uint32_t>(number_property(support, entry, "IOSurfacePlaneElementWidth").value_or(1));
            plane.element_height =
                static_cast<std::uint32_t>(number_property(support, entry, "IOSurfacePlaneElementHeight").value_or(1));
            if (!plane.width || !plane.height || plane.width > maximum_dimension || plane.height > maximum_dimension ||
                !plane.bytes_per_element || plane.bytes_per_element > 64 || !plane.element_width || !plane.element_height)
                return std::nullopt;
            const auto minimum_row = (static_cast<std::uint64_t>(plane.width) + plane.element_width - 1) / plane.element_width *
                                     plane.bytes_per_element;
            plane.bytes_per_row =
                number_property(support, entry, "IOSurfacePlaneBytesPerRow").value_or(align_up(minimum_row, row_alignment));
            if (plane.bytes_per_row < minimum_row) return std::nullopt;
            const auto rows = (static_cast<std::uint64_t>(plane.height) + plane.element_height - 1) / plane.element_height;
            const auto previous_end = planes.empty() ? 0 : planes.back().offset + planes.back().size;
            plane.offset = number_property(support, entry, "IOSurfacePlaneOffset").value_or(align_up(previous_end, row_alignment));
            plane.size = std::max(number_property(support, entry, "IOSurfacePlaneSize").value_or(0), plane.bytes_per_row * rows);
            if (plane.offset > (1ULL << 40) || plane.size > (1ULL << 40)) return std::nullopt;
            planes.push_back(plane);
        }
    }
    return planes;
}

Address create_surface(Support& support, Address cls, Address properties) {
    if (!properties) fail("IOSurfaceCreate: NULL properties");
    auto planes = plane_properties(support, properties);
    if (!planes) return 0;
    auto surface = std::make_shared<Surface>();
    if (!planes->empty()) {
        // A planar surface: the top-level width and height are the surface's, the
        // element and row fields plane 0's; the allocation covers every plane.
        surface->planes = std::move(*planes);
        const auto& first = surface->planes.front();
        surface->width = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfaceWidth").value_or(first.width));
        surface->height = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfaceHeight").value_or(first.height));
        if (!surface->width || !surface->height || surface->width > maximum_dimension || surface->height > maximum_dimension) return 0;
        surface->bytes_per_element = first.bytes_per_element;
        surface->element_width = first.element_width;
        surface->element_height = first.element_height;
        surface->bytes_per_row = first.bytes_per_row;
        surface->pixel_format = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfacePixelFormat").value_or(0));
        std::uint64_t end = 0;
        for (const auto& plane : surface->planes) end = std::max(end, plane.offset + plane.size);
        surface->alloc_size =
            align_up(std::max(number_property(support, properties, "IOSurfaceAllocSize").value_or(0), end), size_alignment);
        surface->base = allocate_pixels(support, surface->alloc_size, surface->vm);
        return make_surface_object(support, cls, surface);
    }
    surface->width = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfaceWidth").value_or(0));
    surface->height = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfaceHeight").value_or(0));
    surface->bytes_per_element = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfaceBytesPerElement").value_or(4));
    surface->element_width = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfaceElementWidth").value_or(1));
    surface->element_height = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfaceElementHeight").value_or(1));
    surface->pixel_format = static_cast<std::uint32_t>(number_property(support, properties, "IOSurfacePixelFormat").value_or(0));
    if (surface->width == 0 || surface->height == 0 || surface->width > maximum_dimension || surface->height > maximum_dimension)
        return 0;   // IOSurfaceCreate returns NULL for impossible sizes
    if (surface->bytes_per_element == 0 || surface->bytes_per_element > 64 || surface->element_width == 0 ||
        surface->element_height == 0)
        return 0;
    const auto elements_per_row = (static_cast<std::uint64_t>(surface->width) + surface->element_width - 1) / surface->element_width;
    const auto minimum_row = elements_per_row * surface->bytes_per_element;
    surface->bytes_per_row = number_property(support, properties, "IOSurfaceBytesPerRow").value_or(align_up(minimum_row, row_alignment));
    if (surface->bytes_per_row < minimum_row) return 0;
    const auto rows = (static_cast<std::uint64_t>(surface->height) + surface->element_height - 1) / surface->element_height;
    const auto minimum_size = surface->bytes_per_row * rows;
    surface->alloc_size = std::max(number_property(support, properties, "IOSurfaceAllocSize").value_or(0), minimum_size);
    surface->alloc_size = align_up(surface->alloc_size, size_alignment);
    surface->base = allocate_pixels(support, surface->alloc_size, surface->vm);
    return make_surface_object(support, cls, surface);
}

} // namespace

std::optional<SurfaceView> surface_view(Support& support, Address object) {
    const auto surface = find_surface(support, object);
    if (!surface) return std::nullopt;
    return SurfaceView{surface->base, surface->width, surface->height, surface->bytes_per_element, surface->bytes_per_row,
                       surface->pixel_format, surface->uses->seed()};
}

void wait_for_surface_gpu_writes(Support& support, Address object) {
    if (const auto surface = find_surface(support, object)) wait_for_gpu_writes(*surface->uses, "layer contents");
}

std::shared_ptr<void> hold_surface_use(Support& support, Address object) {
    const auto surface = find_surface(support, object);
    if (!surface || !surface->uses) return nullptr;
    struct Use {
        std::shared_ptr<SharedState> counter;
        ~Use() { counter->decrement(); }
    };
    auto use = std::make_shared<Use>();
    use->counter = surface->uses;
    use->counter->increment();
    return use;
}

void install_iosurface(Support& support) {
    ProcessContext::add_provider_library(std::string(iosurface_library));
    const auto lib = iosurface_library;
    // IOSurface (the bridged class; IOSurfaceRef is an IOSurface *).
    const auto cls = support.declare_class("IOSurface", "NSObject", lib);
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kIOSurfaceAllocSize", "IOSurfaceAllocSize"}, {"_kIOSurfaceWidth", "IOSurfaceWidth"},
             {"_kIOSurfaceHeight", "IOSurfaceHeight"}, {"_kIOSurfaceBytesPerRow", "IOSurfaceBytesPerRow"},
             {"_kIOSurfaceBytesPerElement", "IOSurfaceBytesPerElement"}, {"_kIOSurfaceElementWidth", "IOSurfaceElementWidth"},
             {"_kIOSurfaceElementHeight", "IOSurfaceElementHeight"}, {"_kIOSurfaceOffset", "IOSurfaceOffset"},
             {"_kIOSurfacePlaneInfo", "IOSurfacePlaneInfo"}, {"_kIOSurfacePlaneWidth", "IOSurfacePlaneWidth"},
             {"_kIOSurfacePlaneHeight", "IOSurfacePlaneHeight"}, {"_kIOSurfacePlaneBytesPerRow", "IOSurfacePlaneBytesPerRow"},
             {"_kIOSurfacePlaneOffset", "IOSurfacePlaneOffset"}, {"_kIOSurfacePlaneSize", "IOSurfacePlaneSize"},
             {"_kIOSurfacePixelFormat", "IOSurfacePixelFormat"}, {"_kIOSurfaceCacheMode", "IOSurfaceCacheMode"},
             {"_kIOSurfaceIsGlobal", "IOSurfaceIsGlobal"}, {"_kIOSurfaceName", "IOSurfaceName"},
             {"_kIOSurfacePixelSizeCastingAllowed", "IOSurfacePixelSizeCastingAllowed"}})
        support.string_constant(lib, symbol, value);

    auto* s = &support;
    support.function(lib, "_IOSurfaceGetTypeID", [](CpuState& c, GuestMemory&) { c.x[0] = 0x100; });
    support.function(lib, "_IOSurfaceCreate", [s, cls](CpuState& c, GuestMemory&) { c.x[0] = create_surface(*s, cls, c.x[0]); });
    support.function(lib, "_IOSurfaceLookup", [s](CpuState& c, GuestMemory&) {
        Address found{};
        {
            auto& table = registry();
            std::lock_guard lock(table.mutex);
            if (const auto entry = table.by_id.find(static_cast<std::uint32_t>(c.x[0])); entry != table.by_id.end())
                found = entry->second;
        }
        c.x[0] = found && s->process.objects().try_retain(found) ? found : 0;
    });
    support.function(lib, "_IOSurfaceGetID", [s](CpuState& c, GuestMemory&) { c.x[0] = require_surface(*s, c.x[0], "IOSurfaceGetID")->id; });
    // mach_port_t IOSurfaceCreateMachPort(IOSurfaceRef) / IOSurfaceRef
    // IOSurfaceLookupFromMachPort(mach_port_t) (+1; NULL for a port that
    // names no surface, MACH_PORT_NULL when no port can be made).
    {
        auto& r = releases();
        r.owner = support.shared_from_this();
        if (main_run_loop_available()) r.source = main_run_loop().add_source(common_modes, [] { drain_releases(); });
    }
    set_mach_kernel_object_receiver(support.process, "IOSurface",
        [s, cls](const std::vector<std::uint8_t>& data, std::uint64_t address, std::uint64_t size) {
            return receive_surface(*s, cls, data, address, size);
        });
    support.function(lib, "_IOSurfaceCreateMachPort", [s](CpuState& c, GuestMemory&) {
        static_cast<void>(require_surface(*s, c.x[0], "IOSurfaceCreateMachPort"));
        const auto ports = mach_kernel_objects(s->process);
        if (!ports) { c.x[0] = 0; return; }
        auto object = std::make_shared<SurfacePort>();
        object->surface = s->retain(c.x[0]);
        object->state = find_surface(*s, c.x[0]);
        c.x[0] = ports->make_port(std::move(object));
    });
    support.function(lib, "_IOSurfaceLookupFromMachPort", [s](CpuState& c, GuestMemory&) {
        const auto ports = mach_kernel_objects(s->process);
        const auto object = ports ? std::dynamic_pointer_cast<SurfacePort>(ports->object(static_cast<std::uint32_t>(c.x[0]))) : nullptr;
        c.x[0] = object && s->process.objects().try_retain(object->surface) ? object->surface : 0;
    });
    support.function(lib, "_IOSurfaceLock", [s](CpuState& c, GuestMemory& m) {
        const auto surface = find_surface(*s, c.x[0]);
        if (!surface) { c.x[0] = kern_invalid_argument; return; }
        // CPU access sees the GPU writes submitted so far (in any process),
        // unless the caller asked not to synchronize.
        if (!(c.x[1] & lock_avoid_sync)) wait_for_gpu_writes(*surface->uses, "IOSurfaceLock");
        ++surface->locks;
        if (c.x[2]) m.write32(c.x[2], surface->uses->seed());
        c.x[0] = kern_success;
    });
    support.function(lib, "_IOSurfaceUnlock", [s](CpuState& c, GuestMemory& m) {
        const auto surface = find_surface(*s, c.x[0]);
        if (!surface || surface->locks.load() <= 0) { c.x[0] = kern_invalid_argument; return; }
        --surface->locks;
        if (!(c.x[1] & lock_read_only)) surface->uses->value->seed.fetch_add(1);
        if (c.x[2]) m.write32(c.x[2], surface->uses->seed());
        c.x[0] = kern_success;
    });
    const auto getter = [s](const char* symbol, auto member) {
        s->function(iosurface_library, symbol, [s, symbol, member](CpuState& c, GuestMemory&) {
            const auto surface = require_surface(*s, c.x[0], symbol + 1);
            c.x[0] = static_cast<std::uint64_t>(member(*surface));
        });
    };
    getter("_IOSurfaceGetAllocSize", [](const Surface& v) { return v.alloc_size; });
    getter("_IOSurfaceGetWidth", [](const Surface& v) { return v.width; });
    getter("_IOSurfaceGetHeight", [](const Surface& v) { return v.height; });
    getter("_IOSurfaceGetBytesPerElement", [](const Surface& v) { return v.bytes_per_element; });
    getter("_IOSurfaceGetBytesPerRow", [](const Surface& v) { return v.bytes_per_row; });
    getter("_IOSurfaceGetElementWidth", [](const Surface& v) { return v.element_width; });
    getter("_IOSurfaceGetElementHeight", [](const Surface& v) { return v.element_height; });
    getter("_IOSurfaceGetBaseAddress", [](const Surface& v) { return v.base; });
    getter("_IOSurfaceGetPixelFormat", [](const Surface& v) { return v.pixel_format; });
    getter("_IOSurfaceGetSeed", [](const Surface& v) { return v.uses->seed(); });
    getter("_IOSurfaceGetPlaneCount", [](const Surface& v) { return static_cast<std::uint64_t>(v.planes.size()); });
    // The use count is the surface's across the process tree (UseCounter):
    // uses taken in any process, including the compositor's while the
    // surface is shown as layer contents (hold_surface_use), show in
    // IOSurfaceGetUseCount / IOSurfaceIsInUse everywhere.
    getter("_IOSurfaceGetUseCount", [](const Surface& v) { return static_cast<std::uint32_t>(v.uses->count()); });
    getter("_IOSurfaceIsInUse", [](const Surface& v) { return v.uses->count() > 0 ? 1 : 0; });
    // IOReturn IOSurfaceSetPurgeable(surface, newState, *oldState):
    // kIOSurfacePurgeableNonVolatile 0, Volatile 1, Empty 2, KeepCurrent 3.
    // Pages are never discarded, so an emptied surface is only volatile here
    // and keeps its contents. Not an IOSurface or another state:
    // kIOReturnBadArgument.
    support.function(lib, "_IOSurfaceSetPurgeable", [s](CpuState& c, GuestMemory& m) {
        constexpr std::uint32_t io_bad_argument = 0xE00002C2;
        const auto surface = find_surface(*s, c.x[0]);
        const auto state = static_cast<std::uint32_t>(c.x[1]);
        if (!surface || state > 3) { c.x[0] = io_bad_argument; return; }
        std::uint32_t old{};
        {
            std::lock_guard lock(surface->mutex);
            old = surface->purgeable;
            if (state != 3) surface->purgeable = state == 2 ? 1 : state;
        }
        if (c.x[2]) m.write32(c.x[2], old);
        c.x[0] = kern_success;
    });
    support.function(lib, "_IOSurfaceIncrementUseCount", [s](CpuState& c, GuestMemory&) {
        require_surface(*s, c.x[0], "IOSurfaceIncrementUseCount")->uses->increment();
    });
    support.function(lib, "_IOSurfaceDecrementUseCount", [s](CpuState& c, GuestMemory&) {
        require_surface(*s, c.x[0], "IOSurfaceDecrementUseCount")->uses->decrement();
    });
    // Plane accessors: a planar surface's planes; of a chunky surface, plane 0
    // describes the whole surface. Another plane index answers 0.
    const auto plane = [s](const char* symbol, auto member) {
        s->function(iosurface_library, symbol, [s, symbol, member](CpuState& c, GuestMemory&) {
            const auto surface = require_surface(*s, c.x[0], symbol + 1);
            if (surface->planes.empty()) {
                const Plane whole{surface->width, surface->height, surface->bytes_per_element, surface->element_width,
                                  surface->element_height, surface->bytes_per_row, 0, surface->alloc_size};
                c.x[0] = c.x[1] == 0 ? static_cast<std::uint64_t>(member(*surface, whole)) : 0;
            } else {
                c.x[0] = c.x[1] < surface->planes.size() ? static_cast<std::uint64_t>(member(*surface, surface->planes[c.x[1]])) : 0;
            }
        });
    };
    plane("_IOSurfaceGetWidthOfPlane", [](const Surface&, const Plane& p) { return p.width; });
    plane("_IOSurfaceGetHeightOfPlane", [](const Surface&, const Plane& p) { return p.height; });
    plane("_IOSurfaceGetBytesPerRowOfPlane", [](const Surface&, const Plane& p) { return p.bytes_per_row; });
    plane("_IOSurfaceGetBytesPerElementOfPlane", [](const Surface&, const Plane& p) { return p.bytes_per_element; });
    plane("_IOSurfaceGetElementWidthOfPlane", [](const Surface&, const Plane& p) { return p.element_width; });
    plane("_IOSurfaceGetElementHeightOfPlane", [](const Surface&, const Plane& p) { return p.element_height; });
    plane("_IOSurfaceGetBaseAddressOfPlane", [](const Surface& v, const Plane& p) { return v.base + p.offset; });
    // Attached values (IOSurfaceSetValue & co).
    support.function(lib, "_IOSurfaceSetValue", [s](CpuState& c, GuestMemory&) {
        // A NULL surface is ignored, as on macOS: Chrome's
        // gfx::IOSurfaceCanSetColorSpace probes a color space by setting its
        // values on a NULL surface (ui/gfx/mac/io_surface.cc).
        if (!c.x[0]) return;
        if (!c.x[1]) fail("IOSurfaceSetValue: NULL key");
        const auto key = s->cf.utf8(c.x[1]);
        const auto surface = require_surface(*s, c.x[0], "IOSurfaceSetValue(key \"" + key + "\")");
        const auto value = s->retain(c.x[2]);
        Address old{};
        {
            std::lock_guard lock(surface->mutex);
            auto& slot = surface->values[key];
            old = std::exchange(slot, value);
        }
        s->release(old);
    });
    support.function(lib, "_IOSurfaceCopyValue", [s](CpuState& c, GuestMemory&) {
        const auto surface = require_surface(*s, c.x[0], "IOSurfaceCopyValue");
        const auto key = c.x[1] ? s->cf.utf8(c.x[1]) : std::string{};
        Address value{};
        {
            std::lock_guard lock(surface->mutex);
            if (const auto found = surface->values.find(key); found != surface->values.end()) value = s->retain(found->second);
        }
        c.x[0] = value;
    });
    support.function(lib, "_IOSurfaceRemoveValue", [s](CpuState& c, GuestMemory&) {
        const auto surface = require_surface(*s, c.x[0], "IOSurfaceRemoveValue");
        const auto key = c.x[1] ? s->cf.utf8(c.x[1]) : std::string{};
        Address old{};
        {
            std::lock_guard lock(surface->mutex);
            if (const auto found = surface->values.find(key); found != surface->values.end()) {
                old = found->second;
                surface->values.erase(found);
            }
        }
        s->release(old);
    });
    support.function(lib, "_IOSurfaceRemoveAllValues", [s](CpuState& c, GuestMemory&) {
        const auto surface = require_surface(*s, c.x[0], "IOSurfaceRemoveAllValues");
        std::map<std::string, Address> values;
        {
            std::lock_guard lock(surface->mutex);
            values.swap(surface->values);
        }
        for (const auto& [key, value] : values) s->release(value);
    });
    support.function(lib, "_IOSurfaceCopyAllValues", [s](CpuState& c, GuestMemory&) {
        const auto surface = require_surface(*s, c.x[0], "IOSurfaceCopyAllValues");
        std::vector<std::pair<Address, Address>> entries;
        {
            std::lock_guard lock(surface->mutex);
            for (const auto& [key, value] : surface->values) entries.emplace_back(s->cf.string(key), value);
        }
        c.x[0] = s->cf.dictionary(entries);
        for (const auto& [key, value] : entries) s->release(key);
    });
    // Property limits. Rows align to 64 bytes and sizes to pages, like the
    // allocation this provider makes.
    const auto alignment_of = [s](Address property) -> std::uint64_t {
        const auto name = property ? s->cf.utf8(property) : std::string{};
        if (name == "IOSurfaceBytesPerRow" || name == "IOSurfacePlaneBytesPerRow") return row_alignment;
        if (name == "IOSurfaceAllocSize" || name == "IOSurfacePlaneSize" || name == "IOSurfaceOffset" ||
            name == "IOSurfacePlaneOffset")
            return size_alignment;
        return 1;
    };
    support.function(lib, "_IOSurfaceGetPropertyAlignment", [alignment_of](CpuState& c, GuestMemory&) { c.x[0] = alignment_of(c.x[0]); });
    support.function(lib, "_IOSurfaceAlignProperty", [alignment_of](CpuState& c, GuestMemory&) {
        c.x[0] = align_up(c.x[1], alignment_of(c.x[0]));
    });
    support.function(lib, "_IOSurfaceGetPropertyMaximum", [s](CpuState& c, GuestMemory&) {
        const auto name = c.x[0] ? s->cf.utf8(c.x[0]) : std::string{};
        if (name == "IOSurfaceWidth" || name == "IOSurfaceHeight") c.x[0] = maximum_dimension;
        else if (name == "IOSurfaceBytesPerRow") c.x[0] = maximum_dimension * 64;
        else c.x[0] = UINT32_MAX;
    });
}

namespace {
// SurfacePages::keep.
struct PagesKeep {
    std::shared_ptr<Pages> pages;
    std::shared_ptr<SharedState> state;
};
const SharedState& shared_of(const quartzcore::SurfacePages& pages) {
    if (!pages.keep) throw std::runtime_error("IOSurface: SurfacePages without its surface state");
    return *static_cast<const PagesKeep*>(pages.keep.get())->state;
}
} // namespace

} // namespace alchemy::gui::support_kit

namespace alchemy::gui::quartzcore {

std::optional<SurfacePages> surface_pages(std::uint64_t surface_object) {
    auto* support = support_kit::support();
    if (!support || !surface_object) return std::nullopt;
    const auto surface = support_kit::find_surface(*support, surface_object);
    if (!surface) return std::nullopt;
    auto keep = std::make_shared<support_kit::PagesKeep>();
    {
        std::lock_guard lock(surface->mutex);
        keep->pages = surface->pages;
    }
    keep->state = surface->uses;
    if (!keep->pages || !keep->state) return std::nullopt;   // freed
    SurfacePages result;
    result.surface = surface_object;
    result.id = surface->id;
    result.width = surface->width;
    result.height = surface->height;
    result.bytes_per_element = surface->bytes_per_element;
    result.pixel_format = surface->pixel_format;
    result.base = surface->base;
    result.bytes_per_row = surface->bytes_per_row;
    result.alloc_size = surface->alloc_size;
    result.shared_memory = surface->vm;
    result.keep = std::move(keep);
    return result;
}

std::uint32_t SurfacePages::seed() const { return support_kit::shared_of(*this).seed(); }

std::uint64_t begin_gpu_write(const SurfacePages& pages) {
    return support_kit::shared_of(pages).value->gpu_writes.fetch_add(1) + 1;
}

std::uint64_t gpu_writes_submitted(const SurfacePages& pages) { return support_kit::shared_of(pages).value->gpu_writes.load(); }

bool wait_for_gpu_writes(const SurfacePages& pages, std::uint64_t ticket, std::uint32_t timeout_ms) {
    return support_kit::shared_of(pages).wait_for_gpu(std::chrono::milliseconds(timeout_ms), ticket);
}

std::uint32_t end_gpu_write(const SurfacePages& pages, std::uint64_t ticket) {
    auto& value = *support_kit::shared_of(pages).value;
    auto written = value.gpu_written.load();
    while (written < ticket && !value.gpu_written.compare_exchange_weak(written, ticket)) {}
    return value.seed.fetch_add(1) + 1;
}

} // namespace alchemy::gui::quartzcore
