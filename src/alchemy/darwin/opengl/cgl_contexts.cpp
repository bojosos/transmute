#include "alchemy/darwin/opengl/opengl.hpp"

#include "alchemy/engine_adapter.hpp"
#include "alchemy/host/gl.hpp"
#include "alchemy/registration_audit.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// CGL over host::gl. Values are those of the public CGLTypes.h / CGLRenderers.h.
//
// - Pixel formats: CGLChoosePixelFormat reads the attribute list and gives one virtual screen
//   (the host GPU) when the host can satisfy it: the legacy profile (OpenGL 2.1 and the
//   fixed-function API, on a compatibility profile context) or a core profile (3.2 and 4.1 on
//   a core profile context of the driver's highest version). Off-screen, full-screen, pbuffer,
//   stereo and software-renderer requests have no match (NULL, as on a Mac without such a
//   renderer).
// - Contexts: real host contexts sharing objects through `share`; retain counts; one current
//   context per thread (CGLSetCurrentContext); parameters and enables are kept as set, and
//   CGLLockContext is a recursive per-context lock. A context has no drawable of its own here:
//   it draws into framebuffer objects, and CGLFlushDrawable flushes.
// - CGLTexImageIOSurface2D uploads the surface's pixels (plane `plane`) into the bound texture
//   at that moment, under a read lock of the surface. On macOS the texture aliases the surface;
//   here it is a copy, so later writes to the surface need another CGLTexImageIOSurface2D.
//   GL_YCBCR_422_APPLE / GL_RGB_422_APPLE data are expanded to RGB without color conversion
//   (Y in green, Cb in blue, Cr in red), as those formats sample.
// - Renderer info: one renderer, the host GPU, with the profile versions the driver has.
// - Unavailable: CGLCopyContext (an explicit stop), pbuffers (CGLCreatePBuffer answers
//   kCGLBadAlloc), off-screen and full-screen drawables (kCGLBadOffScreen, kCGLBadFullScreen),
//   CGLMacro.h dispatch through the context object (the object holds no dispatch table).
namespace alchemy::opengl {
namespace {

using transmute::CpuState;
using transmute::GuestMemory;
using Address = std::uint64_t;

// CGLError.
constexpr std::int32_t no_error = 0, bad_attribute = 10000, bad_property = 10001, bad_pixel_format = 10002,
    bad_renderer_info = 10003, bad_context = 10004, bad_drawable = 10005, bad_display = 10006, bad_state = 10007,
    bad_value = 10008, bad_match = 10009, bad_enumeration = 10010, bad_off_screen = 10011, bad_full_screen = 10012,
    bad_window = 10013, bad_address = 10014, bad_code_module = 10015, bad_alloc = 10016, bad_connection = 10017;

// CGLPixelFormatAttribute.
enum : std::uint32_t {
    pfa_all_renderers = 1, pfa_triple_buffer = 3, pfa_double_buffer = 5, pfa_stereo = 6, pfa_aux_buffers = 7, pfa_color_size = 8,
    pfa_alpha_size = 11, pfa_depth_size = 12, pfa_stencil_size = 13, pfa_accum_size = 14, pfa_minimum_policy = 51,
    pfa_maximum_policy = 52, pfa_off_screen = 53, pfa_full_screen = 54, pfa_sample_buffers = 55, pfa_samples = 56,
    pfa_aux_depth_stencil = 57, pfa_color_float = 58, pfa_multisample = 59, pfa_supersample = 60, pfa_sample_alpha = 61,
    pfa_renderer_id = 70, pfa_single_renderer = 71, pfa_no_recovery = 72, pfa_accelerated = 73, pfa_closest_policy = 74,
    pfa_robust = 75, pfa_backing_store = 76, pfa_mp_safe = 78, pfa_window = 80, pfa_multi_screen = 81, pfa_compliant = 83,
    pfa_display_mask = 84, pfa_pbuffer = 90, pfa_remote_pbuffer = 91, pfa_allow_offline_renderers = 96,
    pfa_accelerated_compute = 97, pfa_opengl_profile = 99, pfa_supports_automatic_graphics_switching = 101,
    pfa_virtual_screen_count = 128, pfa_backing_volatile = 77,
};
// CGLOpenGLProfile.
constexpr std::uint32_t profile_legacy = 0x1000, profile_3_2_core = 0x3200, profile_4_1_core = 0x4100;
// The generic (software) renderer IDs, and this host GPU's renderer ID (Alchemy's own value).
constexpr std::uint32_t renderer_generic = 0x00020200, renderer_generic_float = 0x00020400, renderer_apple_software = 0x00020400;
constexpr std::uint32_t renderer_host = 0x00027000;
constexpr std::uint32_t renderer_id_mask = 0x00FE7F00;
// CGLContextEnable.
constexpr std::uint32_t ce_swap_rectangle = 201, ce_swap_limit = 203, ce_rasterization = 221, ce_state_validation = 301,
    ce_surface_backing_size = 305, ce_display_list_optimization = 307, ce_mp_engine = 313, ce_crash_on_removed_functions = 316;
// CGLContextParameter.
constexpr std::uint32_t cp_swap_rectangle = 200, cp_swap_interval = 222, cp_dispatch_table_size = 224, cp_client_storage = 226,
    cp_surface_texture = 228, cp_surface_order = 235, cp_surface_opacity = 236, cp_surface_backing_size = 304,
    cp_surface_surface_volatile = 306, cp_reclaim_resources = 308, cp_current_renderer_id = 309, cp_gpu_vertex_processing = 310,
    cp_gpu_fragment_processing = 311, cp_has_drawable = 314, cp_mp_swaps_in_flight = 315, cp_gpu_restart_status = 317,
    cp_abort_on_gpu_restart_status_blacklisted = 318, cp_support_gpu_restart = 319, cp_support_separate_address_space = 320,
    cp_context_priority_request = 608;
// CGLGlobalOption.
constexpr std::uint32_t go_format_cache_size = 501, go_clear_format_cache = 502, go_retain_renderers = 503,
    go_reset_library = 504, go_use_error_handler = 505, go_use_build_cache = 506;
// CGLRendererProperty.
constexpr std::uint32_t rp_off_screen = 53, rp_full_screen = 54, rp_renderer_id = 70, rp_accelerated = 73, rp_robust = 75,
    rp_backing_store = 76, rp_mp_safe = 78, rp_window = 80, rp_multi_screen = 81, rp_compliant = 83, rp_display_mask = 84,
    rp_buffer_modes = 100, rp_color_modes = 103, rp_accum_modes = 104, rp_depth_modes = 105, rp_stencil_modes = 106,
    rp_max_aux_buffers = 107, rp_max_sample_buffers = 108, rp_max_samples = 109, rp_sample_modes = 110, rp_sample_alpha = 111,
    rp_gpu_vert_proc_capable = 122, rp_gpu_frag_proc_capable = 123, rp_renderer_count = 128, rp_online = 129,
    rp_accelerated_compute = 130, rp_major_gl_version = 133, rp_removable = 142;
// Buffer, color and depth mode bits (CGLRenderers.h).
constexpr std::uint32_t single_buffer_bit = 0x1, double_buffer_bit = 0x2, rgba8888_bit = 0x00008000, mode_0_bit = 0x1,
    mode_8_bit = 0x20, mode_16_bit = 0x80, mode_24_bit = 0x200, mode_32_bit = 0x400, multisample_bit = 0x2;

// OpenGL values used for IOSurface uploads.
constexpr std::uint32_t gl_unpack_row_length = 0x0CF2, gl_unpack_skip_rows = 0x0CF3, gl_unpack_skip_pixels = 0x0CF4,
    gl_unpack_alignment = 0x0CF5, gl_pixel_unpack_buffer = 0x88EC, gl_pixel_unpack_buffer_binding = 0x88EF;
constexpr std::uint32_t gl_unsigned_byte = 0x1401, gl_unsigned_short = 0x1403, gl_float = 0x1406, gl_half_float = 0x140B,
    gl_unsigned_int_8_8_8_8 = 0x8035, gl_unsigned_int_8_8_8_8_rev = 0x8367, gl_unsigned_int_2_10_10_10_rev = 0x8368,
    gl_unsigned_short_8_8_apple = 0x85BA, gl_unsigned_short_8_8_rev_apple = 0x85BB;
constexpr std::uint32_t gl_red = 0x1903, gl_alpha = 0x1906, gl_rgb = 0x1907, gl_rgba = 0x1908, gl_luminance = 0x1909,
    gl_luminance_alpha = 0x190A, gl_rg = 0x8227, gl_bgra = 0x80E1, gl_ycbcr_422_apple = 0x85B9, gl_rgb_422_apple = 0x8A1F,
    gl_rgb8 = 0x8051;
// kIOSurfaceLockReadOnly.
constexpr std::uint32_t surface_lock_read_only = 1;

constexpr std::string_view iosurface_library = "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface";

void status(CpuState& s, std::int32_t code) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(code)); }

struct PixelFormat {
    host::gl::Format format;
    std::uint32_t profile{profile_legacy};
    std::atomic<std::uint32_t> retains{1};
};

struct Context {
    host::gl::Context* host{};
    Address pixel_format{};            // retained
    Address share_group{};
    std::atomic<std::uint32_t> retains{1};
    std::recursive_mutex lock;
    std::mutex state;
    std::map<std::uint32_t, std::vector<std::int32_t>> parameters;
    std::set<std::uint32_t> enabled;
    std::int32_t virtual_screen{};
};

struct RendererInfo {
    std::uint32_t major{};             // highest core version (0: none)
};

class Cgl : public std::enable_shared_from_this<Cgl> {
public:
    explicit Cgl(gui::InstallContext& context) : process_(context.process) {}
    void install();

private:
    template <class F>
    void fn(const char* name, F handler, const std::source_location& where = std::source_location::current()) {
        registration_audit::Site site(where);
        static_cast<void>(process_.install_function(name,
            [owner = shared_from_this(), handler = std::move(handler)](CpuState& s, GuestMemory& m) { handler(s, m); },
            std::string(opengl_library)));
    }
    Address object(std::uint64_t size = 64) {
        const auto address = process_.allocate_guest(size, 16);
        process_.calling_engine().fill_bytes(address, 0, size);
        return address;
    }
    std::shared_ptr<PixelFormat> pixel_format(Address address) {
        std::lock_guard lock(mutex_);
        const auto found = formats_.find(address);
        return found == formats_.end() ? nullptr : found->second;
    }
    std::shared_ptr<Context> context(Address address) {
        std::lock_guard lock(mutex_);
        const auto found = contexts_.find(address);
        return found == contexts_.end() ? nullptr : found->second;
    }
    std::int32_t choose(GuestMemory& m, Address attributes, Address& out);
    void release_pixel_format(Address address);
    void release_context(Address address);
    std::int32_t describe_pixel_format(const PixelFormat& format, std::uint32_t attribute, std::int32_t& value) const;
    std::int32_t describe_renderer(std::uint32_t property, std::int32_t& value) const;
    std::int32_t texture_from_surface(GuestMemory& m, CpuState& s);
    std::uint64_t guest_call(std::string_view library, std::string_view name, std::initializer_list<std::uint64_t> arguments) {
        const auto found = process_.resolve(library, name);
        if (!found) throw std::runtime_error("Alchemy OpenGL: missing " + std::string(name));
        return EngineAdapter(process_.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
    }

    ProcessContext& process_;
    std::mutex mutex_;
    std::map<Address, std::shared_ptr<PixelFormat>> formats_;
    std::map<Address, std::shared_ptr<Context>> contexts_;
    std::map<host::gl::Context*, Address> share_groups_;   // root host context -> CGLShareGroupObj
    std::map<Address, RendererInfo> renderer_infos_;
    std::map<std::uint32_t, std::int32_t> global_options_{{go_format_cache_size, 5}, {go_retain_renderers, 0},
                                                          {go_use_build_cache, 0}, {go_use_error_handler, 0}};
    std::map<Address, std::string> error_strings_;
};

// The calling thread's current CGL context (the guest's object) per process.
thread_local std::map<const Cgl*, Address> current_contexts;

// Every live CGLContextObj's host context (cgl_host_context).
std::mutex& registry_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::map<Address, host::gl::Context*>& registry() {
    static std::map<Address, host::gl::Context*> map;
    return map;
}

std::int32_t Cgl::choose(GuestMemory& m, Address attributes, Address& out) {
    out = 0;
    if (!attributes) return bad_attribute;
    host::gl::Format format;
    format.alpha_bits = 0;
    format.double_buffer = false;
    std::uint32_t profile = profile_legacy;
    bool impossible = false;
    for (unsigned i = 0; i < 256; ++i) {
        const auto attribute = m.read32(attributes + 4ULL * i);
        if (attribute == 0) break;
        const auto value = [&] { return m.read32(attributes + 4ULL * ++i); };
        switch (attribute) {
        case pfa_double_buffer: case pfa_triple_buffer: format.double_buffer = true; break;
        case pfa_color_size: format.color_bits = std::min(value(), 32U); break;
        case pfa_alpha_size: format.alpha_bits = value(); break;
        case pfa_depth_size: format.depth_bits = value(); break;
        case pfa_stencil_size: format.stencil_bits = value(); break;
        case pfa_samples: format.samples = value(); break;
        case pfa_sample_buffers: if (value() == 0) format.samples = 0; break;
        case pfa_aux_buffers: case pfa_accum_size: if (value() != 0) impossible = true; break;
        case pfa_display_mask: static_cast<void>(value()); break;
        case pfa_virtual_screen_count: static_cast<void>(value()); break;
        case pfa_opengl_profile: {
            const auto requested = value();
            if (requested != profile_legacy && requested != profile_3_2_core && requested != profile_4_1_core) return bad_value;
            profile = requested;
            break;
        }
        case pfa_renderer_id: {
            const auto id = value() & renderer_id_mask;
            if (id == (renderer_generic & renderer_id_mask) || id == (renderer_generic_float & renderer_id_mask) ||
                id == (renderer_apple_software & renderer_id_mask))
                impossible = true;
            else if (id != (renderer_host & renderer_id_mask)) impossible = true;
            break;
        }
        case pfa_stereo: case pfa_off_screen: case pfa_full_screen: case pfa_pbuffer: case pfa_remote_pbuffer: case pfa_color_float:
            impossible = true;
            break;
        case pfa_all_renderers: case pfa_minimum_policy: case pfa_maximum_policy: case pfa_aux_depth_stencil: case pfa_multisample:
        case pfa_supersample: case pfa_sample_alpha: case pfa_single_renderer: case pfa_no_recovery: case pfa_accelerated:
        case pfa_closest_policy: case pfa_robust: case pfa_backing_store: case pfa_backing_volatile: case pfa_mp_safe: case pfa_window:
        case pfa_multi_screen: case pfa_compliant: case pfa_allow_offline_renderers: case pfa_accelerated_compute:
        case pfa_supports_automatic_graphics_switching:
            break;
        default:
            return bad_attribute;
        }
    }
    if (impossible) return no_error;
    format.core = profile != profile_legacy;
    const auto renderer = host::gl::renderer();
    if (!renderer) return no_error;
    if (profile == profile_3_2_core && renderer->major < 3) return no_error;
    if (profile == profile_4_1_core && (renderer->major < 4 || (renderer->major == 4 && renderer->minor < 1))) return no_error;
    auto made = std::make_shared<PixelFormat>();
    made->format = format;
    made->profile = profile;
    out = object();
    std::lock_guard lock(mutex_);
    formats_[out] = std::move(made);
    return no_error;
}

void Cgl::release_pixel_format(Address address) {
    std::shared_ptr<PixelFormat> format;
    {
        std::lock_guard lock(mutex_);
        const auto found = formats_.find(address);
        if (found == formats_.end()) return;
        if (--found->second->retains != 0) return;
        format = std::move(found->second);
        formats_.erase(found);
    }
    process_.release_guest(address);
}

void Cgl::release_context(Address address) {
    std::shared_ptr<Context> gone;
    {
        std::lock_guard lock(mutex_);
        const auto found = contexts_.find(address);
        if (found == contexts_.end()) return;
        if (--found->second->retains != 0) return;
        gone = std::move(found->second);
        contexts_.erase(found);
        for (auto it = share_groups_.begin(); it != share_groups_.end();)
            it = it->first == gone->host ? share_groups_.erase(it) : std::next(it);
    }
    auto& current = current_contexts[this];
    if (current == address) {
        static_cast<void>(host::gl::make_current(nullptr));
        current = 0;
    }
    {
        std::lock_guard lock(registry_mutex());
        registry().erase(address);
    }
    host::gl::destroy_context(gone->host);
    release_pixel_format(gone->pixel_format);
    process_.release_guest(address);
}

std::int32_t Cgl::describe_pixel_format(const PixelFormat& p, std::uint32_t attribute, std::int32_t& value) const {
    const auto& f = p.format;
    switch (attribute) {
    case pfa_double_buffer: value = f.double_buffer; return no_error;
    case pfa_color_size: value = static_cast<std::int32_t>(f.color_bits); return no_error;
    case pfa_alpha_size: value = static_cast<std::int32_t>(f.alpha_bits); return no_error;
    case pfa_depth_size: value = static_cast<std::int32_t>(f.depth_bits); return no_error;
    case pfa_stencil_size: value = static_cast<std::int32_t>(f.stencil_bits); return no_error;
    case pfa_samples: value = static_cast<std::int32_t>(f.samples); return no_error;
    case pfa_sample_buffers: case pfa_multisample: value = f.samples > 1; return no_error;
    case pfa_opengl_profile: value = static_cast<std::int32_t>(p.profile); return no_error;
    case pfa_renderer_id: value = static_cast<std::int32_t>(renderer_host); return no_error;
    case pfa_virtual_screen_count: value = 1; return no_error;
    case pfa_display_mask: value = -1; return no_error;
    case pfa_accelerated: case pfa_window: case pfa_closest_policy: case pfa_mp_safe: case pfa_compliant: case pfa_no_recovery:
    case pfa_supports_automatic_graphics_switching: case pfa_allow_offline_renderers:
        value = 1;
        return no_error;
    case pfa_triple_buffer: case pfa_stereo: case pfa_aux_buffers: case pfa_accum_size: case pfa_minimum_policy:
    case pfa_maximum_policy: case pfa_off_screen: case pfa_full_screen: case pfa_aux_depth_stencil: case pfa_color_float:
    case pfa_supersample: case pfa_sample_alpha: case pfa_single_renderer: case pfa_robust: case pfa_backing_store:
    case pfa_backing_volatile: case pfa_multi_screen: case pfa_pbuffer: case pfa_remote_pbuffer: case pfa_accelerated_compute:
    case pfa_all_renderers:
        value = 0;
        return no_error;
    default:
        return bad_attribute;
    }
}

std::int32_t Cgl::describe_renderer(std::uint32_t property, std::int32_t& value) const {
    const auto renderer = host::gl::renderer();
    switch (property) {
    case rp_renderer_id: value = static_cast<std::int32_t>(renderer_host); return no_error;
    case rp_accelerated: case rp_window: case rp_mp_safe: case rp_compliant: case rp_gpu_vert_proc_capable:
    case rp_gpu_frag_proc_capable: case rp_online:
        value = 1;
        return no_error;
    case rp_off_screen: case rp_full_screen: case rp_robust: case rp_backing_store: case rp_multi_screen: case rp_removable:
    case rp_accelerated_compute: case rp_max_aux_buffers: case rp_sample_alpha:
        value = 0;
        return no_error;
    case rp_display_mask: value = -1; return no_error;
    case rp_renderer_count: value = 1; return no_error;
    case rp_buffer_modes: value = static_cast<std::int32_t>(single_buffer_bit | double_buffer_bit); return no_error;
    case rp_color_modes: value = static_cast<std::int32_t>(rgba8888_bit); return no_error;
    case rp_accum_modes: value = static_cast<std::int32_t>(mode_0_bit); return no_error;
    case rp_depth_modes: value = static_cast<std::int32_t>(mode_0_bit | mode_16_bit | mode_24_bit | mode_32_bit); return no_error;
    case rp_stencil_modes: value = static_cast<std::int32_t>(mode_0_bit | mode_8_bit); return no_error;
    case rp_max_sample_buffers: value = 1; return no_error;
    case rp_max_samples: value = 8; return no_error;
    case rp_sample_modes: value = static_cast<std::int32_t>(multisample_bit); return no_error;
    case rp_major_gl_version: value = renderer ? static_cast<std::int32_t>(std::min(renderer->major, 4U)) : 2; return no_error;
    default: return bad_property;
    }
}

// Bytes per pixel of an upload in `format`/`type` (0: not one CGLTexImageIOSurface2D supports).
unsigned bytes_per_pixel(std::uint32_t format, std::uint32_t type) {
    unsigned components = 0;
    switch (format) {
    case gl_red: case gl_alpha: case gl_luminance: components = 1; break;
    case gl_rg: case gl_luminance_alpha: components = 2; break;
    case gl_rgb: components = 3; break;
    case gl_rgba: case gl_bgra: components = 4; break;
    case gl_ycbcr_422_apple: case gl_rgb_422_apple:
        return type == gl_unsigned_short_8_8_apple || type == gl_unsigned_short_8_8_rev_apple ? 2 : 0;
    default: return 0;
    }
    switch (type) {
    case gl_unsigned_byte: return components;
    case gl_unsigned_short: case gl_half_float: return components * 2;
    case gl_float: return components * 4;
    case gl_unsigned_int_8_8_8_8: case gl_unsigned_int_8_8_8_8_rev: case gl_unsigned_int_2_10_10_10_rev:
        return components == 4 ? 4 : 0;
    default: return 0;
    }
}

std::int32_t Cgl::texture_from_surface(GuestMemory& m, CpuState& s) {
    // (ctx, target, internal_format, width, height, format, type, ioSurface, plane)
    const auto ctx = context(s.x[0]);
    if (!ctx) return bad_context;
    const auto target = static_cast<std::uint32_t>(s.x[1]);
    const auto internal_format = static_cast<std::int32_t>(s.x[2]);
    const auto width = static_cast<std::int32_t>(s.x[3]), height = static_cast<std::int32_t>(s.x[4]);
    const auto format = static_cast<std::uint32_t>(s.x[5]), type = static_cast<std::uint32_t>(s.x[6]);
    const auto surface = s.x[7];
    const auto plane = static_cast<std::uint32_t>(m.read32(s.sp));
    if (!surface || width <= 0 || height <= 0) return bad_value;
    const auto bpp = bytes_per_pixel(format, type);
    if (!bpp) return bad_value;
    const bool planar = guest_call(iosurface_library, "_IOSurfaceGetPlaneCount", {surface}) != 0;
    const auto base = planar ? guest_call(iosurface_library, "_IOSurfaceGetBaseAddressOfPlane", {surface, plane})
                             : guest_call(iosurface_library, "_IOSurfaceGetBaseAddress", {surface});
    const auto row = planar ? guest_call(iosurface_library, "_IOSurfaceGetBytesPerRowOfPlane", {surface, plane})
                            : guest_call(iosurface_library, "_IOSurfaceGetBytesPerRow", {surface});
    const auto surface_height = planar ? guest_call(iosurface_library, "_IOSurfaceGetHeightOfPlane", {surface, plane})
                                       : guest_call(iosurface_library, "_IOSurfaceGetHeight", {surface});
    if (!base || row < static_cast<std::uint64_t>(width) * bpp || surface_height < static_cast<std::uint64_t>(height)) return bad_value;

    // The context must be current for the upload; restore the caller's afterwards.
    auto& current = current_contexts[this];
    const auto previous = current;
    auto* previous_host = host::gl::current();
    if (previous != s.x[0] && !host::gl::make_current(ctx->host)) return bad_context;
    using GetInteger = void (*)(std::uint32_t, std::int32_t*);
    using PixelStore = void (*)(std::uint32_t, std::int32_t);
    using BindBuffer = void (*)(std::uint32_t, std::uint32_t);
    using TexImage = void (*)(std::uint32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::uint32_t,
                              std::uint32_t, const void*);
    const auto get_integer = gl_function<GetInteger>("glGetIntegerv");
    const auto pixel_store = gl_function<PixelStore>("glPixelStorei");
    const auto bind_buffer = gl_function<BindBuffer>("glBindBuffer");
    const auto tex_image = gl_function<TexImage>("glTexImage2D");
    std::int32_t result = no_error;
    if (!get_integer || !pixel_store || !tex_image) {
        result = bad_context;
    } else {
        static_cast<void>(guest_call(iosurface_library, "_IOSurfaceLock", {surface, surface_lock_read_only, 0}));
        std::array<std::int32_t, 4> saved{};
        constexpr std::array<std::uint32_t, 4> unpack{gl_unpack_row_length, gl_unpack_skip_rows, gl_unpack_skip_pixels, gl_unpack_alignment};
        for (std::size_t i = 0; i < unpack.size(); ++i) get_integer(unpack[i], &saved[i]);
        std::int32_t unpack_buffer{};
        if (bind_buffer) {
            get_integer(gl_pixel_unpack_buffer_binding, &unpack_buffer);
            if (unpack_buffer) bind_buffer(gl_pixel_unpack_buffer, 0);
        }
        pixel_store(gl_unpack_skip_rows, 0);
        pixel_store(gl_unpack_skip_pixels, 0);
        if (format == gl_ycbcr_422_apple || format == gl_rgb_422_apple) {
            // Two pixels per 4 bytes: Cb Y0 Cr Y1 (8_8), or Y0 Cb Y1 Cr (8_8_REV).
            const bool reversed = type == gl_unsigned_short_8_8_rev_apple;
            std::vector<std::uint8_t> source(static_cast<std::size_t>(row) * static_cast<std::size_t>(height));
            m.read_bytes(base, source);
            std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
            for (std::int32_t y = 0; y < height; ++y) {
                const auto* line = source.data() + static_cast<std::size_t>(y) * row;
                auto* out = rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3;
                for (std::int32_t x = 0; x < width; ++x) {
                    const auto* pair = line + (x / 2) * 4;
                    const std::uint8_t cb = reversed ? pair[1] : pair[0], cr = reversed ? pair[3] : pair[2];
                    const std::uint8_t luma = reversed ? pair[(x & 1) ? 2 : 0] : pair[(x & 1) ? 3 : 1];
                    out[x * 3] = cr;
                    out[x * 3 + 1] = luma;
                    out[x * 3 + 2] = cb;
                }
            }
            pixel_store(gl_unpack_row_length, 0);
            pixel_store(gl_unpack_alignment, 1);
            tex_image(target, 0, internal_format == static_cast<std::int32_t>(gl_rgb_422_apple) ? static_cast<std::int32_t>(gl_rgb8) : internal_format,
                      width, height, 0, gl_rgb, gl_unsigned_byte, rgb.data());
        } else {
            const bool whole_pixels = row % bpp == 0;
            pixel_store(gl_unpack_row_length, whole_pixels ? static_cast<std::int32_t>(row / bpp) : 0);
            pixel_store(gl_unpack_alignment, 1);
            if (whole_pixels) {
                // Guest addresses are host addresses (alchemy-fx): the driver reads the pages.
                tex_image(target, 0, internal_format, width, height, 0, format, type, reinterpret_cast<const void*>(base));
            } else {
                std::vector<std::uint8_t> packed(static_cast<std::size_t>(width) * bpp * static_cast<std::size_t>(height));
                for (std::int32_t y = 0; y < height; ++y)
                    m.read_bytes(base + static_cast<std::uint64_t>(y) * row,
                                 std::span(packed.data() + static_cast<std::size_t>(y) * width * bpp, static_cast<std::size_t>(width) * bpp));
                tex_image(target, 0, internal_format, width, height, 0, format, type, packed.data());
            }
        }
        for (std::size_t i = 0; i < unpack.size(); ++i) pixel_store(unpack[i], saved[i]);
        if (unpack_buffer && bind_buffer) bind_buffer(gl_pixel_unpack_buffer, static_cast<std::uint32_t>(unpack_buffer));
        static_cast<void>(guest_call(iosurface_library, "_IOSurfaceUnlock", {surface, surface_lock_read_only, 0}));
    }
    if (previous != s.x[0]) static_cast<void>(host::gl::make_current(previous_host));
    return result;
}

void Cgl::install() {
    ProcessContext::add_provider_library(std::string(opengl_library));
    auto self = this;

    // ---- pixel formats ----
    fn("_CGLChoosePixelFormat", [self](CpuState& s, GuestMemory& m) {
        Address made{};
        const auto code = self->choose(m, s.x[0], made);
        if (s.x[1]) m.write64(s.x[1], made);
        if (s.x[2]) m.write32(s.x[2], made ? 1 : 0);
        status(s, code);
    });
    for (const auto* name : {"_CGLDestroyPixelFormat", "_CGLReleasePixelFormat"}) {
        const bool destroy = std::string_view(name) == "_CGLDestroyPixelFormat";
        fn(name, [self, destroy](CpuState& s, GuestMemory&) {
            const bool known = s.x[0] && self->pixel_format(s.x[0]);
            if (known) self->release_pixel_format(s.x[0]);
            if (destroy) status(s, known ? no_error : bad_pixel_format);
        });
    }
    fn("_CGLRetainPixelFormat", [self](CpuState& s, GuestMemory&) {
        if (const auto format = self->pixel_format(s.x[0])) ++format->retains;
    });
    fn("_CGLGetPixelFormatRetainCount", [self](CpuState& s, GuestMemory&) {
        const auto format = self->pixel_format(s.x[0]);
        s.x[0] = format ? format->retains.load() : 0;
    });
    fn("_CGLDescribePixelFormat", [self](CpuState& s, GuestMemory& m) {
        const auto format = self->pixel_format(s.x[0]);
        if (!format) return status(s, bad_pixel_format);
        if (static_cast<std::int32_t>(s.x[1]) != 0) return status(s, bad_value);
        if (!s.x[3]) return status(s, bad_address);
        std::int32_t value{};
        const auto code = self->describe_pixel_format(*format, static_cast<std::uint32_t>(s.x[2]), value);
        if (code == no_error) m.write32(s.x[3], static_cast<std::uint32_t>(value));
        status(s, code);
    });

    // ---- renderer info ----
    fn("_CGLQueryRendererInfo", [self](CpuState& s, GuestMemory& m) {
        // (GLuint display_mask, CGLRendererInfoObj *rend, GLint *nrend)
        if (!s.x[1] || !s.x[2]) return status(s, bad_address);
        const auto renderer = host::gl::renderer();
        if (!renderer) {
            m.write64(s.x[1], 0);
            m.write32(s.x[2], 0);
            return status(s, bad_display);
        }
        const auto info = self->object();
        {
            std::lock_guard lock(self->mutex_);
            self->renderer_infos_[info] = {renderer->major};
        }
        m.write64(s.x[1], info);
        m.write32(s.x[2], 1);
        status(s, no_error);
    });
    fn("_CGLDestroyRendererInfo", [self](CpuState& s, GuestMemory&) {
        bool known{};
        {
            std::lock_guard lock(self->mutex_);
            known = self->renderer_infos_.erase(s.x[0]) != 0;
        }
        if (known) self->process_.release_guest(s.x[0]);
        status(s, known ? no_error : bad_renderer_info);
    });
    fn("_CGLDescribeRenderer", [self](CpuState& s, GuestMemory& m) {
        // (CGLRendererInfoObj rend, GLint rend_num, CGLRendererProperty prop, GLint *value)
        {
            std::lock_guard lock(self->mutex_);
            if (!self->renderer_infos_.contains(s.x[0])) return status(s, bad_renderer_info);
        }
        if (static_cast<std::int32_t>(s.x[1]) != 0) return status(s, bad_value);
        if (!s.x[3]) return status(s, bad_address);
        std::int32_t value{};
        const auto code = self->describe_renderer(static_cast<std::uint32_t>(s.x[2]), value);
        if (code == no_error) m.write32(s.x[3], static_cast<std::uint32_t>(value));
        status(s, code);
    });

    // ---- contexts ----
    fn("_CGLCreateContext", [self](CpuState& s, GuestMemory& m) {
        // (CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj *ctx)
        if (s.x[2]) m.write64(s.x[2], 0);
        const auto format = self->pixel_format(s.x[0]);
        if (!format) return status(s, bad_pixel_format);
        if (!s.x[2]) return status(s, bad_address);
        std::shared_ptr<Context> share;
        if (s.x[1] && !(share = self->context(s.x[1]))) return status(s, bad_context);
        if (share) {
            const auto share_format = self->pixel_format(share->pixel_format);
            if (share_format && (share_format->profile == profile_legacy) != (format->profile == profile_legacy)) return status(s, bad_match);
        }
        auto created = host::gl::create_context(format->format, share ? share->host : nullptr);
        if (!created) return status(s, created.error() == host::Error::invalid_argument ? bad_match : bad_alloc);
        auto made = std::make_shared<Context>();
        made->host = *created;
        made->pixel_format = s.x[0];
        made->enabled.insert(ce_rasterization);   // on until disabled
        ++format->retains;
        const auto address = self->object(256);
        {
            std::lock_guard lock(self->mutex_);
            if (share) {
                made->share_group = share->share_group;
            } else {
                made->share_group = self->object();
                self->share_groups_[made->host] = made->share_group;
            }
            self->contexts_[address] = std::move(made);
        }
        {
            std::lock_guard lock(registry_mutex());
            registry()[address] = *created;
        }
        m.write64(s.x[2], address);
        status(s, no_error);
    });
    fn("_CGLDestroyContext", [self](CpuState& s, GuestMemory&) {
        if (!self->context(s.x[0])) return status(s, bad_context);
        self->release_context(s.x[0]);
        status(s, no_error);
    });
    fn("_CGLRetainContext", [self](CpuState& s, GuestMemory&) {
        if (const auto ctx = self->context(s.x[0])) ++ctx->retains;
    });
    fn("_CGLReleaseContext", [self](CpuState& s, GuestMemory&) { self->release_context(s.x[0]); });
    fn("_CGLGetContextRetainCount", [self](CpuState& s, GuestMemory&) {
        const auto ctx = self->context(s.x[0]);
        s.x[0] = ctx ? ctx->retains.load() : 0;
    });
    fn("_CGLGetPixelFormat", [self](CpuState& s, GuestMemory&) {
        const auto ctx = self->context(s.x[0]);
        s.x[0] = ctx ? ctx->pixel_format : 0;
    });
    fn("_CGLGetShareGroup", [self](CpuState& s, GuestMemory&) {
        const auto ctx = self->context(s.x[0]);
        s.x[0] = ctx ? ctx->share_group : 0;
    });
    fn("_CGLSetCurrentContext", [self](CpuState& s, GuestMemory&) {
        auto& current = current_contexts[self];
        if (!s.x[0]) {
            static_cast<void>(host::gl::make_current(nullptr));
            current = 0;
            return status(s, no_error);
        }
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        if (!host::gl::make_current(ctx->host)) return status(s, bad_context);
        current = s.x[0];
        status(s, no_error);
    });
    fn("_CGLGetCurrentContext", [self](CpuState& s, GuestMemory&) {
        const auto found = current_contexts.find(self);
        s.x[0] = found == current_contexts.end() ? 0 : found->second;
    });
    fn("_CGLLockContext", [self](CpuState& s, GuestMemory&) {
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        ctx->lock.lock();
        status(s, no_error);
    });
    fn("_CGLUnlockContext", [self](CpuState& s, GuestMemory&) {
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        ctx->lock.unlock();
        status(s, no_error);
    });
    fn("_CGLFlushDrawable", [self](CpuState& s, GuestMemory&) {
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        // No drawable is attached to a CGL context here, so flushing is all there is.
        const auto& current = current_contexts[self];
        if (current == s.x[0]) {
            if (const auto flush = gl_function<void (*)()>("glFlush")) flush();
        }
        status(s, no_error);
    });
    fn("_CGLClearDrawable", [self](CpuState& s, GuestMemory&) { status(s, self->context(s.x[0]) ? no_error : bad_context); });
    fn("_CGLUpdateContext", [self](CpuState& s, GuestMemory&) { status(s, self->context(s.x[0]) ? no_error : bad_context); });
    fn("_CGLSetVirtualScreen", [self](CpuState& s, GuestMemory&) {
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        status(s, static_cast<std::int32_t>(s.x[1]) == 0 ? no_error : bad_value);
    });
    fn("_CGLGetVirtualScreen", [self](CpuState& s, GuestMemory& m) {
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        if (!s.x[1]) return status(s, bad_address);
        m.write32(s.x[1], 0);
        status(s, no_error);
    });
    for (const auto* name : {"_CGLEnable", "_CGLDisable"}) {
        const bool enable = std::string_view(name) == "_CGLEnable";
        fn(name, [self, enable](CpuState& s, GuestMemory&) {
            const auto ctx = self->context(s.x[0]);
            if (!ctx) return status(s, bad_context);
            const auto pname = static_cast<std::uint32_t>(s.x[1]);
            switch (pname) {
            case ce_swap_rectangle: case ce_swap_limit: case ce_rasterization: case ce_state_validation: case ce_surface_backing_size:
            case ce_display_list_optimization: case ce_mp_engine: case ce_crash_on_removed_functions:
                break;
            default:
                return status(s, bad_enumeration);
            }
            std::lock_guard lock(ctx->state);
            if (enable) ctx->enabled.insert(pname);
            else ctx->enabled.erase(pname);
            status(s, no_error);
        });
    }
    fn("_CGLIsEnabled", [self](CpuState& s, GuestMemory& m) {
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        if (!s.x[2]) return status(s, bad_address);
        const auto pname = static_cast<std::uint32_t>(s.x[1]);
        std::lock_guard lock(ctx->state);
        m.write32(s.x[2], ctx->enabled.contains(pname) ? 1 : 0);
        status(s, no_error);
    });
    fn("_CGLSetParameter", [self](CpuState& s, GuestMemory& m) {
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        const auto pname = static_cast<std::uint32_t>(s.x[1]);
        if (!s.x[2]) return status(s, bad_address);
        unsigned count = 1;
        switch (pname) {
        case cp_swap_rectangle: count = 4; break;
        case cp_surface_backing_size: count = 2; break;
        case cp_swap_interval: case cp_client_storage: case cp_surface_order: case cp_surface_opacity:
        case cp_surface_surface_volatile: case cp_reclaim_resources: case cp_mp_swaps_in_flight:
        case cp_abort_on_gpu_restart_status_blacklisted: case cp_context_priority_request:
            break;
        case cp_current_renderer_id: case cp_gpu_vertex_processing: case cp_gpu_fragment_processing: case cp_has_drawable:
        case cp_gpu_restart_status: case cp_support_gpu_restart: case cp_support_separate_address_space: case cp_dispatch_table_size:
            return status(s, bad_enumeration);   // read-only
        case cp_surface_texture:
            return status(s, bad_drawable);      // no surface drawable exists here
        default:
            return status(s, bad_enumeration);
        }
        std::vector<std::int32_t> values(count);
        for (unsigned i = 0; i < count; ++i) values[i] = static_cast<std::int32_t>(m.read32(s.x[2] + 4ULL * i));
        std::lock_guard lock(ctx->state);
        ctx->parameters[pname] = std::move(values);
        status(s, no_error);
    });
    fn("_CGLGetParameter", [self](CpuState& s, GuestMemory& m) {
        const auto ctx = self->context(s.x[0]);
        if (!ctx) return status(s, bad_context);
        const auto pname = static_cast<std::uint32_t>(s.x[1]);
        if (!s.x[2]) return status(s, bad_address);
        std::vector<std::int32_t> values;
        {
            std::lock_guard lock(ctx->state);
            if (const auto found = ctx->parameters.find(pname); found != ctx->parameters.end()) values = found->second;
        }
        if (values.empty()) {
            switch (pname) {
            case cp_swap_rectangle: values = {0, 0, 0, 0}; break;
            case cp_surface_backing_size: values = {0, 0}; break;
            case cp_swap_interval: case cp_client_storage: case cp_surface_surface_volatile: case cp_reclaim_resources:
            case cp_has_drawable: case cp_gpu_restart_status: case cp_support_gpu_restart: case cp_support_separate_address_space:
            case cp_abort_on_gpu_restart_status_blacklisted: case cp_context_priority_request: case cp_mp_swaps_in_flight:
                values = {0};
                break;
            case cp_surface_order: case cp_surface_opacity: case cp_gpu_vertex_processing: case cp_gpu_fragment_processing:
                values = {1};
                break;
            case cp_current_renderer_id: values = {static_cast<std::int32_t>(renderer_host)}; break;
            default: return status(s, bad_enumeration);
            }
        }
        for (std::size_t i = 0; i < values.size(); ++i) m.write32(s.x[2] + 4ULL * i, static_cast<std::uint32_t>(values[i]));
        status(s, no_error);
    });
    const auto option_known = [](std::uint32_t pname) {
        return pname == go_format_cache_size || pname == go_clear_format_cache || pname == go_retain_renderers ||
            pname == go_reset_library || pname == go_use_error_handler || pname == go_use_build_cache;
    };
    for (const auto* name : {"_CGLSetGlobalOption", "_CGLSetOption"}) {
        fn(name, [self, option_known](CpuState& s, GuestMemory&) {
            const auto pname = static_cast<std::uint32_t>(s.x[0]);
            if (!option_known(pname)) return status(s, bad_enumeration);
            std::lock_guard lock(self->mutex_);
            if (pname != go_clear_format_cache && pname != go_reset_library)
                self->global_options_[pname] = static_cast<std::int32_t>(s.x[1]);
            status(s, no_error);
        });
    }
    for (const auto* name : {"_CGLGetGlobalOption", "_CGLGetOption"}) {
        fn(name, [self, option_known](CpuState& s, GuestMemory& m) {
            const auto pname = static_cast<std::uint32_t>(s.x[0]);
            if (!option_known(pname)) return status(s, bad_enumeration);
            if (!s.x[1]) return status(s, bad_address);
            std::lock_guard lock(self->mutex_);
            const auto found = self->global_options_.find(pname);
            m.write32(s.x[1], found == self->global_options_.end() ? 0 : static_cast<std::uint32_t>(found->second));
            status(s, no_error);
        });
    }
    fn("_CGLGetVersion", [](CpuState& s, GuestMemory& m) {
        if (s.x[0]) m.write32(s.x[0], 1);
        if (s.x[1]) m.write32(s.x[1], 2);
    });
    fn("_CGLErrorString", [self](CpuState& s, GuestMemory& m) {
        static const std::map<std::int32_t, std::string_view> texts{
            {no_error, "no error"}, {bad_attribute, "invalid pixel format attribute"}, {bad_property, "invalid renderer property"},
            {bad_pixel_format, "invalid pixel format"}, {bad_renderer_info, "invalid renderer info"}, {bad_context, "invalid context"},
            {bad_drawable, "invalid drawable"}, {bad_display, "invalid graphics device"}, {bad_state, "invalid context state"},
            {bad_value, "invalid numerical value"}, {bad_match, "invalid share context"}, {bad_enumeration, "invalid enumerant"},
            {bad_off_screen, "invalid offscreen drawable"}, {bad_full_screen, "invalid fullscreen drawable"},
            {bad_window, "invalid window"}, {bad_address, "invalid pointer"}, {bad_code_module, "invalid code module"},
            {bad_alloc, "invalid memory allocation"}, {bad_connection, "invalid CoreGraphics connection"}};
        const auto found = texts.find(static_cast<std::int32_t>(s.x[0]));
        const auto text = found == texts.end() ? std::string_view("unknown error") : found->second;
        std::lock_guard lock(self->mutex_);
        static std::map<const Cgl*, std::map<std::string_view, Address>> cache;
        auto& slot = cache[self][text];
        if (!slot) {
            slot = self->process_.allocate_guest(text.size() + 1, 1);
            std::vector<std::uint8_t> bytes(text.begin(), text.end());
            bytes.push_back(0);
            m.write_bytes(slot, bytes);
        }
        s.x[0] = slot;
    });
    fn("_CGLTexImageIOSurface2D", [self](CpuState& s, GuestMemory& m) { status(s, self->texture_from_surface(m, s)); });

    // ---- unavailable ----
    fn("_CGLCopyContext", [](CpuState&, GuestMemory&) {
        throw std::runtime_error("unsupported: Alchemy: CGLCopyContext (copying context state between contexts is not provided)");
    });
    fn("_CGLCreatePBuffer", [](CpuState& s, GuestMemory& m) {
        // (width, height, target, internalFormat, max_level, CGLPBufferObj *pbuffer)
        if (s.x[5]) m.write64(s.x[5], 0);
        status(s, bad_alloc);
    });
    for (const auto* name : {"_CGLDestroyPBuffer", "_CGLDescribePBuffer", "_CGLTexImagePBuffer", "_CGLSetPBuffer", "_CGLGetPBuffer"})
        fn(name, [](CpuState& s, GuestMemory&) { status(s, bad_address); });   // no pbuffer can exist
    fn("_CGLRetainPBuffer", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    fn("_CGLReleasePBuffer", [](CpuState&, GuestMemory&) {});
    fn("_CGLGetPBufferRetainCount", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    fn("_CGLSetOffScreen", [](CpuState& s, GuestMemory&) { status(s, bad_off_screen); });
    fn("_CGLGetOffScreen", [](CpuState& s, GuestMemory& m) {
        // (ctx, GLsizei *width, GLsizei *height, GLsizei *rowbytes, void **baseaddr): none attached.
        for (unsigned i = 1; i <= 3; ++i)
            if (s.x[i]) m.write32(s.x[i], 0);
        if (s.x[4]) m.write64(s.x[4], 0);
        status(s, no_error);
    });
    fn("_CGLSetFullScreen", [](CpuState& s, GuestMemory&) { status(s, bad_full_screen); });
    fn("_CGLSetFullScreenOnDisplay", [](CpuState& s, GuestMemory&) { status(s, bad_full_screen); });
}

} // namespace

host::gl::Context* cgl_host_context(std::uint64_t cgl_context) {
    std::lock_guard lock(registry_mutex());
    const auto found = registry().find(cgl_context);
    return found == registry().end() ? nullptr : found->second;
}

void install_cgl(gui::InstallContext& context) { std::make_shared<Cgl>(context)->install(); }

} // namespace alchemy::opengl
