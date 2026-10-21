#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/darwin/quartzcore/layer_tree.hpp"
#include "alchemy/darwin/quartzcore/presenter.hpp"
#include "alchemy/darwin/quartzcore/quartzcore.hpp"

#include "alchemy/gui/layer_contents.hpp"
#include "alchemy/gui/runloop.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

// CALayer trees of view backing layers composited into their windows and
// presented through WindowServer::present_bitmap (see
// include/alchemy/gui/layer_contents.hpp). QuartzCore keeps the layer model
// and tells this file what changed (quartzcore::observe_layer_changes) and
// when a transaction commits (observe_transaction_commits); the composition
// itself is layer_tree.hpp.
//
// When: the trees changed inside an explicit CATransaction are composited
// when its outermost +commit returns on the main thread; changes outside one
// (the implicit transaction) and changes on other threads at the next
// main-run-loop step. AppKit's display of a view that updates its layer
// (layer_contents::view_layer_displayed) composites that view's tree at once.
//
// What: a tree is composited when its root is the backing layer of a view in
// a visible window and the tree shows contents somewhere (an IOSurface or a
// CGImage on any layer: Qt's QNSView sets its own layer's, Chrome's
// DisplayCALayerTree a sublayer's). Other trees (layer-backed views AppKit draws itself, CAMetalLayer views) are
// left to AppKit and QuartzCore's presenter as before. Each window keeps one
// composite of all such trees; a change recomposites the area of the trees it
// touched (their old and new frames) and presents that area; an area no
// composited tree covers any more is handed back to AppKit (an expose).
//
// Supported contents: IOSurfaces in 'BGRA' (premultiplied B,G,R,A bytes:
// Chrome's software compositor and Qt's QCALayerBackingStore) and CGImages
// (decoded through the CoreGraphics provider into premultiplied BGRA). Other
// IOSurface pixel formats are reported once on stderr and not drawn.
namespace alchemy::gui::support_kit {
namespace {

constexpr std::uint32_t format_bgra = 0x42475241;   // 'BGRA'
constexpr double retry_seconds = 0.25;               // trees waiting for their window to show

struct Composite {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> bgra;
    std::uint64_t frames{};
    std::chrono::steady_clock::time_point last_capture{};
    TimerId trailing_capture{};   // writes a frame the throttle held back
};

// A composited tree: where it was last shown.
struct Root {
    WindowId window{};
    Rect frame;          // content points, bottom-left origin
    std::uint64_t order{};
};

struct State {
    std::mutex mutex;                         // dirty, signalled, uses (any thread)
    std::set<Address> dirty;                  // roots changed since the last composition
    bool signalled{};
    std::map<Address, std::shared_ptr<void>> uses;   // layer -> the use of its IOSurface contents
    SourceId source{};
    // Main thread only.
    std::map<WindowId, Composite> windows;
    std::map<Address, Root> roots;
    std::set<Address> waiting;                // trees whose window is not visible yet
    TimerId retry{};
    std::uint64_t next_order{};
    std::uint64_t compositions{};
    bool format_reported{}, transform_reported{};
};
State& state() { static State value; return value; }

std::atomic<WindowServer*> server_override{nullptr};
WindowServer* server_or_null() noexcept {
    if (const auto value = server_override.load()) return value;
    return window_server_available() ? &window_server() : nullptr;
}

const std::filesystem::path& capture_directory() {
    static const std::filesystem::path directory = [] {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "ALCHEMY_CONTENTS_CAPTURE") != 0 || value == nullptr) return std::filesystem::path{};
        std::filesystem::path result(value);
        std::free(value);
        if (!result.empty()) std::filesystem::create_directories(result);
        return result;
    }();
    return directory;
}

void write_capture_files(WindowId window, const Composite& composite);

// Writes the capture of `window` (throttled: the first three frames, then at
// most every 250 ms, a held-back frame when its 250 ms are up; the final
// state of every window is written at exit). Main thread.
void write_capture(WindowId window, Composite& composite) {
    const auto& directory = capture_directory();
    if (directory.empty()) return;
    static const bool exit_hook = [] {
        std::atexit([] {
            try {
                for (const auto& [id, value] : state().windows) write_capture_files(id, value);
            } catch (...) {}
        });
        return true;
    }();
    static_cast<void>(exit_hook);
    const auto now = std::chrono::steady_clock::now();
    if (composite.frames > 3 && now - composite.last_capture < std::chrono::milliseconds(250)) {
        if (!composite.trailing_capture && main_run_loop_available()) {
            const auto wait = std::chrono::duration<double>(composite.last_capture + std::chrono::milliseconds(250) - now).count();
            composite.trailing_capture = main_run_loop().add_timer(monotonic_seconds() + std::max(0.0, wait), 0, common_modes, [window](TimerId) {
                const auto found = state().windows.find(window);
                if (found == state().windows.end()) return;
                found->second.trailing_capture = 0;
                found->second.last_capture = std::chrono::steady_clock::now();
                write_capture_files(window, found->second);
            });
        }
        return;
    }
    composite.last_capture = now;
    write_capture_files(window, composite);
}

void write_capture_files(WindowId window, const Composite& composite) {
    const auto& directory = capture_directory();
    if (directory.empty() || composite.bgra.empty()) return;
    const auto stem = "window-" + std::to_string(window);
    const auto path = directory / (stem + ".bmp");
    const auto temporary = directory / (stem + ".bmp.tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::uint32_t image_size = composite.width * composite.height * 4;
        const std::uint32_t file_size = 54 + image_size;
        std::uint8_t header[54]{};
        header[0] = 'B'; header[1] = 'M';
        std::memcpy(header + 2, &file_size, 4);
        const std::uint32_t offset = 54, info_size = 40;
        std::memcpy(header + 10, &offset, 4);
        std::memcpy(header + 14, &info_size, 4);
        const std::int32_t width = static_cast<std::int32_t>(composite.width), height = -static_cast<std::int32_t>(composite.height);
        std::memcpy(header + 18, &width, 4);
        std::memcpy(header + 22, &height, 4);
        const std::uint16_t planes = 1, bits = 32;
        std::memcpy(header + 26, &planes, 2);
        std::memcpy(header + 28, &bits, 2);
        std::memcpy(header + 34, &image_size, 4);
        out.write(reinterpret_cast<const char*>(header), sizeof header);
        out.write(reinterpret_cast<const char*>(composite.bgra.data()), static_cast<std::streamsize>(composite.bgra.size()));
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    std::ofstream info(directory / (stem + ".txt"), std::ios::trunc);
    info << "frames " << composite.frames << " width " << composite.width << " height " << composite.height << "\n";
}

// Host copies of contents pixels (premultiplied BGRA rows, top first).
struct Pixels {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> bgra;
    bool opaque{};
};

bool all_opaque(const std::vector<std::uint8_t>& bgra) {
    for (std::size_t i = 3; i < bgra.size(); i += 4)
        if (bgra[i] != 255) return false;
    return true;
}

std::optional<Pixels> read_surface(Support& support, const SurfaceView& surface) {
    if (surface.pixel_format != format_bgra && !(surface.pixel_format == 0 && surface.bytes_per_element == 4)) {
        auto& s = state();
        if (!s.format_reported) {
            s.format_reported = true;
            char text[128];
            std::snprintf(text, sizeof text, "unsupported: layer contents IOSurface pixel format 0x%08x (only 'BGRA' is composited)",
                          surface.pixel_format);
            diagnostic(text);
        }
        return std::nullopt;
    }
    Pixels pixels{surface.width, surface.height, {}, false};
    pixels.bgra.resize(static_cast<std::size_t>(surface.width) * surface.height * 4);
    auto& memory = support.memory();
    const auto row_bytes = static_cast<std::size_t>(surface.width) * 4;
    if (surface.bytes_per_row == row_bytes) {
        memory.read_bytes(surface.base, pixels.bgra);
    } else {
        for (std::uint32_t y = 0; y < surface.height; ++y)
            memory.read_bytes(surface.base + y * surface.bytes_per_row, std::span(pixels.bgra.data() + y * row_bytes, row_bytes));
    }
    pixels.opaque = all_opaque(pixels.bgra);
    return pixels;
}

std::optional<Pixels> read_image(Support& support, Address image) {
    const auto width = support.call(core_graphics_library, "_CGImageGetWidth", {image});
    const auto height = support.call(core_graphics_library, "_CGImageGetHeight", {image});
    if (width == 0 || height == 0 || width > 32768 || height > 32768) return std::nullopt;
    const auto bytes = width * height * 4;
    const auto buffer = support.process.allocate_guest(bytes, 64);
    struct Free { ProcessContext& process; Address address; ~Free() { process.release_guest(address); } } owned{support.process, buffer};
    support.memory().fill_bytes(buffer, 0, bytes);
    const auto space = support.call(core_graphics_library, "_CGColorSpaceCreateDeviceRGB", {});
    constexpr std::uint64_t info = 2 /* premultiplied first */ | 0x2000 /* 32 little */;
    const auto context = support.call(core_graphics_library, "_CGBitmapContextCreate", {buffer, width, height, 8, width * 4, space, info});
    support.call(core_graphics_library, "_CGColorSpaceRelease", {space});
    if (!context) fail("layer contents: CGBitmapContextCreate failed for a CGImage");
    CallRegs draw;
    draw.x[0] = context;
    draw.x[1] = image;
    draw.set_d(0, 0);
    draw.set_d(1, 0);
    draw.set_d(2, static_cast<double>(width));
    draw.set_d(3, static_cast<double>(height));
    static_cast<void>(support.call(core_graphics_library, "_CGContextDrawImage", draw));
    support.call(core_graphics_library, "_CGContextRelease", {context});
    Pixels pixels{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), {}, false};
    pixels.bgra.resize(static_cast<std::size_t>(bytes));
    support.memory().read_bytes(buffer, pixels.bgra);
    pixels.opaque = all_opaque(pixels.bgra);
    return pixels;
}

std::optional<Pixels> read_contents(Support& support, Address contents) {
    if (auto surface = surface_view(support, contents)) {
        // The window server's implicit synchronization: GPU writes to the
        // surface (IOSurface-backed MTLTextures, in this or the GPU process)
        // finish before the composition reads its pages.
        wait_for_surface_gpu_writes(support, contents);
        surface = surface_view(support, contents);
        if (surface) return read_surface(support, *surface);
    }
    const auto image_class = support.objc.find_class("_AlchemyCGImage");
    if (image_class && support.objc.object_class(contents) == image_class) return read_image(support, contents);
    return std::nullopt;
}

Rect rect_result(const CallResult& result) { return {{result.d(0), result.d(1)}, {result.d(2), result.d(3)}}; }

// A reference held while the compositor messages an object it does not own
// (a layer's delegate is not retained); empty when the object is gone. The
// liveness check is the object store's (ObjectStore::try_retain: an address
// with no live record, or one being destroyed, takes no reference), as for
// the roots and contents below.
struct Held {
    Support& support;
    Address object{};
    Held(Support& owner, Address value) : support(owner), object(value && owner.process.objects().try_retain(value) ? value : 0) {}
    ~Held() { if (object) support.release(object); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    explicit operator bool() const { return object != 0; }
};

// Where the tree at `root` is shown: the view whose backing layer it is, the
// window and the view's frame in content points. A layer's delegate is not
// retained: one that was freed (a view whose layer the app keeps) or a view
// without a window places nothing.
struct Placement { WindowId window{}; Rect frame; };
std::optional<Placement> placement(Support& support, Address root) {
    const Held view_ref(support, support.send(root, "delegate").x0);
    if (!view_ref) return std::nullopt;
    const auto view = view_ref.object;
    if (!support.objc.responds(view, support.objc.selector("convertRect:toView:"))) return std::nullopt;
    if (support.send(view, "layer").x0 != root) return std::nullopt;
    const Held window_ref(support, support.send(view, "window").x0);
    if (!window_ref) return std::nullopt;
    const auto window = window_ref.object;
    const auto id = static_cast<WindowId>(support.send(window, "windowNumber").x0);
    if (!id) return std::nullopt;
    CallRegs convert;
    const auto bounds = rect_result(support.send(view, "bounds"));
    convert.set_d(0, bounds.origin.x);
    convert.set_d(1, bounds.origin.y);
    convert.set_d(2, bounds.size.width);
    convert.set_d(3, bounds.size.height);
    convert.x[2] = 0;
    const auto rect = rect_result(support.send(view, "convertRect:toView:", convert));
    if (!(rect.size.width > 0) || !(rect.size.height > 0)) return std::nullopt;
    return Placement{id, rect};
}

bool shows_contents(const quartzcore::tree::Layer& layer) {
    if (layer.contents_object) return true;
    return std::any_of(layer.sublayers.begin(), layer.sublayers.end(), shows_contents);
}

// Resolves every contents object of the tree into host pixels (`keep` owns them).
void resolve(Support& support, quartzcore::tree::Layer& layer, std::vector<std::unique_ptr<Pixels>>& keep) {
    if (layer.contents_object && !layer.hidden && support.process.objects().try_retain(layer.contents_object)) {
        struct Release { Support& support; Address object; ~Release() { support.release(object); } } held{support, layer.contents_object};
        if (auto pixels = read_contents(support, layer.contents_object)) {
            keep.push_back(std::make_unique<Pixels>(std::move(*pixels)));
            const auto& value = *keep.back();
            layer.contents = quartzcore::tree::Image{value.width, value.height, static_cast<std::size_t>(value.width) * 4,
                                                     value.bgra.data(), value.opaque};
        }
    }
    if (layer.hidden) return;
    for (auto& child : layer.sublayers) resolve(support, child, keep);
}

// ALCHEMY_TRACE_LAYERS=1: the first composites of each window, and every
// 50th, print their trees (geometry, visibility, contents and a sample of their pixels).
bool layer_trace_enabled() {
    static const bool enabled = [] {
        char* value = nullptr;
        std::size_t length = 0;
        const bool set = _dupenv_s(&value, &length, "ALCHEMY_TRACE_LAYERS") == 0 && value && *value && *value != '0';
        std::free(value);
        return set;
    }();
    return enabled;
}
void dump_layer(const quartzcore::tree::Layer& layer, unsigned depth) {
    std::string line(depth * 2, ' ');
    char text[320];
    std::snprintf(text, sizeof text, "layer bounds %.0f,%.0f %.0fx%.0f position %.1f,%.1f anchor %.2f,%.2f opacity %.2f%s%s%s",
                  layer.bounds.origin.x, layer.bounds.origin.y, layer.bounds.size.width, layer.bounds.size.height, layer.position.x,
                  layer.position.y, layer.anchor.x, layer.anchor.y, layer.opacity, layer.hidden ? " hidden" : "",
                  layer.masks_to_bounds ? " masks" : "", layer.geometry_flipped ? " flipped" : "");
    line += text;
    if (layer.background) {
        std::snprintf(text, sizeof text, " background %.2f,%.2f,%.2f,%.2f", (*layer.background)[0], (*layer.background)[1],
                      (*layer.background)[2], (*layer.background)[3]);
        line += text;
    }
    if (layer.contents) {
        const auto& image = *layer.contents;
        std::uint64_t lit = 0, samples = 0;
        for (std::uint32_t y = 0; y < image.height; y += 16)
            for (std::uint32_t x = 0; x < image.width; x += 16, ++samples) {
                const auto* pixel = image.bgra + y * image.stride + x * 4;
                if (pixel[0] + pixel[1] + pixel[2] > 30) ++lit;
            }
        std::snprintf(text, sizeof text, " contents %ux%u%s (%llu of %llu samples lit)", image.width, image.height,
                      image.opaque ? " opaque" : "", static_cast<unsigned long long>(lit), static_cast<unsigned long long>(samples));
        line += text;
    } else if (layer.contents_object) line += " contents (unresolved)";
    std::fprintf(stderr, "[alchemy layers] %s\n", line.c_str());
    for (const auto& child : layer.sublayers) dump_layer(child, depth + 1);
}

Rect unite(const Rect& a, const Rect& b) {
    if (!(a.size.width > 0) || !(a.size.height > 0)) return b;
    if (!(b.size.width > 0) || !(b.size.height > 0)) return a;
    const auto x0 = std::min(a.origin.x, b.origin.x), y0 = std::min(a.origin.y, b.origin.y);
    const auto x1 = std::max(a.origin.x + a.size.width, b.origin.x + b.size.width);
    const auto y1 = std::max(a.origin.y + a.size.height, b.origin.y + b.size.height);
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}
bool overlaps(const Rect& a, const Rect& b) {
    return a.origin.x < b.origin.x + b.size.width && b.origin.x < a.origin.x + a.size.width &&
           a.origin.y < b.origin.y + b.size.height && b.origin.y < a.origin.y + a.size.height;
}

// Recomposites `area` (content points) of `window` from every tree shown in
// it and presents that area.
void composite_window(Support& support, WindowServer& server, WindowId window, Rect area) {
    auto& s = state();
    const auto content = server.content_rect(window).size;
    const auto scale = server.backing_scale(window);
    const auto width = static_cast<std::uint32_t>(std::max(1.0, std::ceil(content.width * scale - 1e-6)));
    const auto height = static_cast<std::uint32_t>(std::max(1.0, std::ceil(content.height * scale - 1e-6)));
    auto& composite = s.windows[window];
    if (composite.width != width || composite.height != height) {
        composite.width = width;
        composite.height = height;
        composite.bgra.assign(static_cast<std::size_t>(width) * height * 4, 0);
        area = {{0, 0}, content};
    }
    namespace tree = quartzcore::tree;
    const tree::PixelRect clip{std::max(0L, static_cast<long>(std::floor(area.origin.x * scale))),
                               std::max(0L, static_cast<long>(std::floor((content.height - area.origin.y - area.size.height) * scale))),
                               std::min(static_cast<long>(width), static_cast<long>(std::ceil((area.origin.x + area.size.width) * scale))),
                               std::min(static_cast<long>(height), static_cast<long>(std::ceil((content.height - area.origin.y) * scale)))};
    if (clip.empty()) return;
    for (long y = clip.y0; y < clip.y1; ++y)
        std::memset(composite.bgra.data() + (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(clip.x0)) * 4, 0,
                    static_cast<std::size_t>(clip.x1 - clip.x0) * 4);
    std::vector<std::pair<std::uint64_t, Address>> order;
    for (const auto& [root, info] : s.roots)
        if (info.window == window && overlaps(info.frame, area)) order.emplace_back(info.order, root);
    std::sort(order.begin(), order.end());
    if (order.empty()) {
        // No composited tree is left there: AppKit draws the area again.
        server.invalidate(window, area);
        return;
    }
    tree::Target target{width, height, static_cast<std::size_t>(width) * 4, composite.bgra.data()};
    for (const auto& [rank, root] : order) {
        static_cast<void>(rank);
        if (!support.process.objects().try_retain(root)) continue;
        struct Release { Support& support; Address object; ~Release() { support.release(object); } } held{support, root};
        auto snapshot = quartzcore::snapshot_tree(support.objc, root);
        std::vector<std::unique_ptr<Pixels>> keep;
        resolve(support, snapshot, keep);
        if (layer_trace_enabled() && (composite.frames < 3 || composite.frames % 50 == 0)) dump_layer(snapshot, 0);
        const auto stats = tree::compose(snapshot, {s.roots[root].frame, content.height, scale}, target, clip);
        if (stats.unsupported_transforms && !s.transform_reported) {
            s.transform_reported = true;
            diagnostic("unsupported: a CALayer with a rotating or shearing transform is not composited (layer and sublayers skipped)");
        }
    }
    const WindowServer::BitmapView view{width, height, static_cast<std::size_t>(width) * 4, composite.bgra.data()};
    const Rect presented{{clip.x0 / scale, content.height - clip.y1 / scale}, {(clip.x1 - clip.x0) / scale, (clip.y1 - clip.y0) / scale}};
    server.present_bitmap(window, view, presented);
    ++composite.frames;
    ++s.compositions;
    if (quartzcore::present_trace_enabled() && (composite.frames <= 3 || composite.frames % 300 == 0)) {
        std::fprintf(stderr, "[alchemy layer-tree] window %llu frame %llu: %zu tree(s), %ux%u, area %.0f,%.0f %.0fx%.0f\n",
                     static_cast<unsigned long long>(window), static_cast<unsigned long long>(composite.frames), order.size(),
                     width, height, presented.origin.x, presented.origin.y, presented.size.width, presented.size.height);
        std::fflush(stderr);
    }
    write_capture(window, composite);
}

void schedule_retry(Support& support);

// Composites the trees at `roots` (main thread).
void composite_roots(Support& support, const std::set<Address>& roots) {
    const auto server = server_or_null();
    if (!server) return;
    auto& s = state();
    std::map<WindowId, Rect> damage;
    const auto forget = [&](Address root) {
        const auto found = s.roots.find(root);
        if (found == s.roots.end()) return;
        damage[found->second.window] = unite(damage[found->second.window], found->second.frame);
        s.roots.erase(found);
    };
    for (const auto root : roots) {
        s.waiting.erase(root);
        if (!support.process.objects().try_retain(root)) { forget(root); continue; }
        struct Release { Support& support; Address object; ~Release() { support.release(object); } } held{support, root};
        if (!quartzcore::is_layer(support.objc, root)) continue;
        if (!shows_contents(quartzcore::snapshot_tree(support.objc, root))) { forget(root); continue; }
        const auto where = placement(support, root);
        if (!where) { forget(root); continue; }
        if (!server->is_visible(where->window)) {
            s.waiting.insert(root);
            continue;
        }
        if (const auto found = s.roots.find(root); found != s.roots.end()) {
            damage[found->second.window] = unite(damage[found->second.window], found->second.frame);
            found->second.window = where->window;
            found->second.frame = where->frame;
        } else {
            s.roots[root] = Root{where->window, where->frame, s.next_order++};
        }
        damage[where->window] = unite(damage[where->window], where->frame);
    }
    for (const auto& [window, area] : damage) {
        if (!server->window_exists(window)) {
            s.windows.erase(window);   // closed: nothing to hand back
            continue;
        }
        if (server->is_visible(window)) composite_window(support, *server, window, area);
    }
    if (!s.waiting.empty()) schedule_retry(support);
}

void flush(Support& support) {
    std::set<Address> roots;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.signalled = false;
        roots.swap(s.dirty);
    }
    if (!roots.empty()) composite_roots(support, roots);
}

// The next main-run-loop step flushes (any thread; coalesced).
void schedule() {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (s.signalled || !s.source) return;
        s.signalled = true;
    }
    main_run_loop().signal_source(s.source);
}

void schedule_retry(Support& support) {
    auto& s = state();
    if (s.retry || !main_run_loop_available()) return;
    auto* sp = &support;
    s.retry = main_run_loop().add_timer(monotonic_seconds() + retry_seconds, 0, common_modes, [sp](TimerId) {
        auto& state_ = state();
        state_.retry = 0;
        const auto waiting = state_.waiting;
        composite_roots(*sp, waiting);
    });
}

} // namespace

void install_layer_contents(Support& support) {
    const auto layer_class = support.objc.find_class("CALayer");
    if (!layer_class) fail("QuartzCore CALayer is missing (install_quartzcore must run first)");
    auto* sp = &support;
    if (main_run_loop_available())
        state().source = main_run_loop().add_source(common_modes, [sp] { flush(*sp); });
    // Any change marks its tree; the outermost commit on the main thread
    // composites at once, everything else at the next run-loop step.
    quartzcore::observe_layer_changes("layer tree compositing", [sp](Address layer) {
        const auto root = quartzcore::root_layer(sp->objc, layer);
        {
            auto& s = state();
            std::lock_guard lock(s.mutex);
            s.dirty.insert(root);
        }
        if (sp->on_main_thread() && quartzcore::in_explicit_transaction()) return;
        schedule();
    });
    quartzcore::observe_transaction_commits("layer tree compositing", [sp] {
        if (sp->on_main_thread()) flush(*sp);
        else schedule();
    });
    // The window server's use of shown contents: an IOSurface that is a
    // layer's contents is in use (IOSurfaceIsInUse, in every process) until
    // the layer drops it, so a producer that cycles a pool of surfaces does
    // not draw into the one being shown (Chrome's software output device).
    quartzcore::observe_layer_contents("window server use of layer contents", [sp](Address layer, Address contents) {
        auto use = contents ? hold_surface_use(*sp, contents) : nullptr;
        std::shared_ptr<void> dropped;   // released outside the lock
        auto& s = state();
        std::lock_guard lock(s.mutex);
        auto& slot = s.uses[layer];
        dropped = std::exchange(slot, std::move(use));
        if (!slot) s.uses.erase(layer);
    });
}

} // namespace alchemy::gui::support_kit

namespace alchemy::gui::layer_contents {

bool view_updates_layer(ObjcRuntime& objc, std::uint64_t view) {
    if (!view) return false;
    const auto wants = objc.send(view, objc.selector("wantsLayer")).x0 & 0xff;
    if (!wants) return false;
    const auto update = objc.send_if_responds(view, objc.selector("wantsUpdateLayer"));
    if (update && (update->x0 & 0xff) != 0) return true;
    if (objc.responds(view, objc.selector("displayLayer:"))) return true;
    // A view whose layer tree this compositor shows (main thread).
    const auto layer = objc.send(view, objc.selector("layer")).x0;
    return layer && support_kit::state().roots.contains(layer);
}

void update_view_layer(ObjcRuntime& objc, std::uint64_t view) {
    const auto layer = objc.send(view, objc.selector("layer")).x0;   // a layer-backed view has its layer when displayed
    const auto update = objc.send_if_responds(view, objc.selector("wantsUpdateLayer"));
    if (update && (update->x0 & 0xff) != 0) {
        static_cast<void>(objc.send(view, objc.selector("updateLayer")));
    } else if (objc.responds(view, objc.selector("displayLayer:"))) {
        CallRegs registers;
        registers.x[2] = layer;
        static_cast<void>(objc.send(view, objc.selector("displayLayer:"), registers));
    }
    view_layer_displayed(objc, view);
}

void view_layer_displayed(ObjcRuntime& objc, std::uint64_t view) {
    auto* support = support_kit::support();
    if (!support || !view) return;
    const auto layer = objc.send(view, objc.selector("layer")).x0;
    if (!layer) return;
    {
        auto& s = support_kit::state();
        std::lock_guard lock(s.mutex);
        s.dirty.erase(layer);
    }
    support_kit::composite_roots(*support, {layer});
}

WindowServer* presentation_server() noexcept { return support_kit::server_or_null(); }

void set_window_server_override(WindowServer* server) noexcept { support_kit::server_override.store(server); }

std::uint64_t compositions() noexcept { return support_kit::state().compositions; }

} // namespace alchemy::gui::layer_contents
