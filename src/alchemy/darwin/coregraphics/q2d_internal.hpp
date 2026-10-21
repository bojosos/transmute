#pragma once

// Quartz 2D drawing layer (owner: Q, mac/q-quartz): shared state of
// src/alchemy/quartz2d, src/alchemy/coretext and src/alchemy/imageio.
// Not a contract header: other subsystems use the layer through the exported
// C functions, Objective-C messages and src/alchemy/darwin/coregraphics/quartz2d.hpp.
//
// Model. Every CoreGraphics / CoreText / ImageIO object (CGContextRef,
// CGPathRef, CGColorRef, CGImageRef, CTFontRef, CTLineRef, ...) is a CF-type
// object in the process object store whose class is a host class of this
// layer, so CFRetain/CFRelease/-retain/-release/-autorelease all work. Host
// state is the object's payload (ObjectStore::create); destruction work that
// may run guest code (release callbacks, CFRelease of retained values) runs
// in the record's destroy function, never in a payload destructor.
//
// Coordinates. "User space" is the CGContext user space (y up). "Base space"
// is the default user space of a context (y up, origin at the bottom left,
// one unit per device pixel for bitmap contexts). "Device space" is the
// Direct2D pixel space of the backing surface (y down, origin at the top
// left): device = (x, height - y) of base space.
//
// Threading. CoreGraphics objects may be used from any guest thread; a
// context is used by one thread at a time (as on macOS) but a recursive mutex
// guards each context anyway. Direct2D / DirectWrite / WIC factories are
// process-wide and thread-safe (multi-threaded D2D factory, shared DWrite
// factory, free-threaded WIC factory).

#include "alchemy/core_foundation.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/window_server.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"
#include "alchemy/registration_audit.hpp"

#include <array>
#include <source_location>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::quartz2d {

using Address = std::uint64_t;
using transmute::CpuState;
using transmute::GuestMemory;
using Handler = transmute::GuestCallHandler;

inline constexpr std::string_view appkit_library = darwin_framework_libraries[0];
inline constexpr std::string_view core_graphics_library = darwin_framework_libraries[7];
inline constexpr std::string_view core_text_library = "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText";
inline constexpr std::string_view image_io_library = "/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO";

[[noreturn]] void fail(std::string_view why);
// "unsupported: <what>" (the explicit failure for behaviour this layer does not implement).
[[noreturn]] void unsupported(std::string_view what);
// Diagnostic line on stderr, like the messages macOS CoreGraphics logs for
// invalid arguments ("<Error>: CGBitmapContextCreate: unsupported parameter combination").
void diagnostic(std::string_view text);

// ---- geometry ----------------------------------------------------------------------
inline constexpr double cg_null_origin = std::numeric_limits<double>::infinity();
inline constexpr double cg_infinite_size = std::numeric_limits<double>::max();
inline constexpr double cg_infinite_origin = -cg_infinite_size / 2;

[[nodiscard]] inline bool rect_is_null(const Rect& r) { return std::isinf(r.origin.x) && r.origin.x > 0; }
[[nodiscard]] inline Rect null_rect() { return {{cg_null_origin, cg_null_origin}, {0, 0}}; }
[[nodiscard]] inline Rect infinite_rect() { return {{cg_infinite_origin, cg_infinite_origin}, {cg_infinite_size, cg_infinite_size}}; }
[[nodiscard]] inline bool rect_is_infinite(const Rect& r) {
    return r.origin.x == cg_infinite_origin && r.origin.y == cg_infinite_origin && r.size.width == cg_infinite_size &&
           r.size.height == cg_infinite_size;
}
[[nodiscard]] Rect standardize(Rect r);
[[nodiscard]] inline double min_x(const Rect& r) { return standardize(r).origin.x; }
[[nodiscard]] inline double min_y(const Rect& r) { return standardize(r).origin.y; }
[[nodiscard]] inline double max_x(const Rect& r) { const auto s = standardize(r); return s.origin.x + s.size.width; }
[[nodiscard]] inline double max_y(const Rect& r) { const auto s = standardize(r); return s.origin.y + s.size.height; }
[[nodiscard]] bool rect_is_empty(const Rect& r);
[[nodiscard]] Rect rect_union(const Rect& a, const Rect& b);
[[nodiscard]] Rect rect_intersection(const Rect& a, const Rect& b);
[[nodiscard]] Rect rect_integral(const Rect& r);
[[nodiscard]] Rect rect_inset(const Rect& r, double dx, double dy);
[[nodiscard]] bool rect_contains_point(const Rect& r, Point p);
[[nodiscard]] Rect rect_from_points(Point a, Point b);

// CGAffineTransform {a, b, c, d, tx, ty}: x' = a x + c y + tx, y' = b x + d y + ty.
struct Affine {
    double a{1}, b{}, c{}, d{1}, tx{}, ty{};
    [[nodiscard]] Point apply(Point p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    [[nodiscard]] Point apply_vector(Point p) const { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
    // `*this` followed by `next` (CGAffineTransformConcat(*this, next)).
    [[nodiscard]] Affine then(const Affine& next) const {
        return {a * next.a + b * next.c, a * next.b + b * next.d, c * next.a + d * next.c, c * next.b + d * next.d,
                tx * next.a + ty * next.c + next.tx, tx * next.b + ty * next.d + next.ty};
    }
    [[nodiscard]] double determinant() const { return a * d - b * c; }
    // CGAffineTransformInvert: the transform itself when it is singular.
    [[nodiscard]] std::optional<Affine> inverse() const;
    [[nodiscard]] bool is_identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }
    [[nodiscard]] bool operator==(const Affine&) const = default;
    // Geometric mean of the axis scales (line width and blur scaling).
    [[nodiscard]] double scale() const { return std::sqrt(std::abs(determinant())); }
    [[nodiscard]] bool axis_aligned() const { return (b == 0 && c == 0) || (a == 0 && d == 0); }
    [[nodiscard]] static Affine translation(double x, double y) { return {1, 0, 0, 1, x, y}; }
    [[nodiscard]] static Affine scaling(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    [[nodiscard]] static Affine rotation(double angle) {
        const auto cosine = std::cos(angle), sine = std::sin(angle);
        return {cosine, sine, -sine, cosine, 0, 0};
    }
};
[[nodiscard]] Rect transform_rect(const Affine& t, const Rect& r);   // bounding box (CGRectApplyAffineTransform)

// ---- paths ---------------------------------------------------------------------------
enum class PathOp : std::uint32_t { move = 0, line = 1, quad = 2, cubic = 3, close = 4 };   // CGPathElementType
struct PathElement {
    PathOp op{};
    std::array<Point, 3> points{};
    [[nodiscard]] unsigned count() const { return op == PathOp::move || op == PathOp::line ? 1 : op == PathOp::quad ? 2 : op == PathOp::cubic ? 3 : 0; }
};
struct PathData {
    std::vector<PathElement> elements;
    Point current{}, start{};
    bool has_current{};

    [[nodiscard]] bool empty() const { return elements.empty(); }
    void move_to(Point p);
    void line_to(Point p);
    void quad_to(Point c, Point p);
    void cubic_to(Point c1, Point c2, Point p);
    void close();
    // Appends with a transform applied (m may be identity).
    void add_rect(const Affine& m, const Rect& r);
    void add_ellipse(const Affine& m, const Rect& r);
    void add_rounded_rect(const Affine& m, const Rect& r, double rx, double ry);
    // CGPathAddArc: angles in radians, `clockwise` in the path's (y up) space.
    void add_arc(const Affine& m, Point center, double radius, double start, double end, bool clockwise);
    void add_relative_arc(const Affine& m, Point center, double radius, double start, double delta);
    void add_arc_to_point(const Affine& m, Point p1, Point p2, double radius);
    void append(const PathData& other, const Affine& m);
    [[nodiscard]] PathData transformed(const Affine& m) const;
    // Control-point box (CGPathGetBoundingBox) and the tight box of the
    // curves (CGPathGetPathBoundingBox). CGRectNull for an empty path.
    [[nodiscard]] Rect control_bounds() const;
    [[nodiscard]] Rect path_bounds() const;
    // Flattened polygons (one per subpath, implicitly closed), tolerance in path units.
    [[nodiscard]] std::vector<std::vector<Point>> flatten(double tolerance) const;
    [[nodiscard]] bool contains(Point p, bool even_odd) const;
    // The rectangle when the path is exactly one axis-aligned closed rectangle.
    [[nodiscard]] std::optional<Rect> as_rect() const;
    [[nodiscard]] bool operator==(const PathData& other) const;
};

// ---- colors --------------------------------------------------------------------------
// CGColorSpaceModel
enum class ColorModel : std::int32_t { unknown = -1, monochrome = 0, rgb = 1, cmyk = 2, lab = 3, device_n = 4, indexed = 5, pattern = 6, xyz = 7 };
struct ColorSpaceInfo final : ObjectPayload {
    ColorModel model{ColorModel::rgb};
    std::string name;                 // "" for device spaces
    bool linear{}, extended{}, wide{};
    Address base{};                   // +1 (pattern / indexed base space)
    std::vector<std::uint8_t> table;  // indexed: (last_index + 1) * base components bytes
    std::uint32_t last_index{};
    std::vector<std::uint8_t> icc;    // ICC profile bytes when created from one
    std::string profile;              // the named space whose generated profile `icc` is (icc_profiles.hpp)
    [[nodiscard]] unsigned components() const;
};
// Straight (unpremultiplied) sRGB color, 0..1.
struct Rgba {
    double r{}, g{}, b{}, a{1};
    [[nodiscard]] bool operator==(const Rgba&) const = default;
};
struct ColorInfo final : ObjectPayload {
    Address space{};                  // +1 CGColorSpaceRef
    std::vector<double> components;   // including alpha (pattern colors: the tint components + alpha)
    Address pattern{};                // +1 CGPatternRef for pattern colors
    Address guest_components{};       // CGColorGetComponents storage (engine heap)
    bool guest_heap{};
    [[nodiscard]] double alpha() const { return components.empty() ? 1 : components.back(); }
};

// ---- images --------------------------------------------------------------------------
// CGImageAlphaInfo / CGBitmapInfo
namespace alpha_info {
inline constexpr std::uint32_t none = 0, premultiplied_last = 1, premultiplied_first = 2, last = 3, first = 4,
    none_skip_last = 5, none_skip_first = 6, only = 7, mask = 0x1f;
}
namespace bitmap_info {
inline constexpr std::uint32_t float_components = 1U << 8, byte_order_mask = 0x7000, order_default = 0,
    order_16_little = 0x1000, order_32_little = 0x2000, order_16_big = 0x3000, order_32_big = 0x4000;
}

// The memory layout of bitmap pixels (bitmap contexts, images, NSBitmapImageRep).
struct PixelLayout {
    std::uint32_t width{}, height{};
    std::uint64_t bytes_per_row{};
    std::uint32_t bits_per_component{8}, bits_per_pixel{32};
    std::uint32_t bitmap_info{};          // CGBitmapInfo (alpha info | byte order | float)
    ColorModel model{ColorModel::rgb};
    unsigned color_components{3};         // from the color space (indexed: 1)
    bool straight_alpha{};                // alpha first/last is not premultiplied (images, NSBitmapImageRep)
    [[nodiscard]] std::uint32_t alpha() const { return bitmap_info & alpha_info::mask; }
    [[nodiscard]] std::uint32_t byte_order() const { return bitmap_info & bitmap_info::byte_order_mask; }
    [[nodiscard]] bool has_alpha() const {
        const auto value = alpha();
        return value != alpha_info::none && value != alpha_info::none_skip_first && value != alpha_info::none_skip_last;
    }
    [[nodiscard]] std::uint64_t byte_size() const { return bytes_per_row * height; }
};
// A bitmap context layout CGBitmapContextCreate accepts (8-bit RGB with
// premultiplied or skipped alpha, 8-bit gray, 8-bit alpha only), or the
// reason it does not.
[[nodiscard]] std::optional<std::string> check_context_layout(const PixelLayout& layout);

// Host pixel buffer in Direct2D's format: premultiplied BGRA8, rows top to
// bottom, stride width * 4.
struct Pbgra {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> pixels;
    [[nodiscard]] std::uint8_t* row(std::uint32_t y) { return pixels.data() + static_cast<std::size_t>(y) * width * 4; }
    [[nodiscard]] const std::uint8_t* row(std::uint32_t y) const { return pixels.data() + static_cast<std::size_t>(y) * width * 4; }
};
// Converts `count` pixels of one row of a context-layout buffer (accepted by
// check_context_layout, or 8-bit RGBA/RGB with straight alpha) to / from
// premultiplied BGRA.
void row_to_pbgra(const PixelLayout& layout, const std::uint8_t* row, std::uint32_t count, std::uint8_t* out);
void pbgra_to_row(const PixelLayout& layout, const std::uint8_t* in, std::uint32_t count, std::uint8_t* row);
[[nodiscard]] inline std::uint32_t pixel_bytes(const PixelLayout& layout) { return layout.bits_per_pixel / 8; }

struct ImageDesc {
    PixelLayout layout;
    std::vector<double> decode;           // CGImage decode array (empty: default)
    bool mask{};                          // CGImageMaskCreate image (1 component, no color space)
    std::optional<Rgba> index_lookup_unused;
    std::vector<std::uint8_t> palette;    // indexed color space table (RGB triples after conversion)
};
// Decodes image samples (1..16-bit integer or 32-bit float components, gray /
// RGB / CMYK / indexed, any alpha placement and byte order) to premultiplied
// BGRA. Masks decode to alpha only (sample 0 paints, as CGImageMaskCreate).
[[nodiscard]] Pbgra decode_samples(const ImageDesc& desc, std::span<const std::uint8_t> bytes);

struct DataProviderInfo final : ObjectPayload {
    // Either host bytes, or guest memory owned by the guest with a release callback.
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    Address guest_data{}, guest_size{};
    Address release_callback{}, release_info{};   // CGDataProviderReleaseDataCallback(info, data, size)
    Address cf_data{};                            // +1 CFData (CGDataProviderCreateWithCFData)
    // Snapshot of the provider's bytes (copies guest memory on each call).
    [[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> snapshot(GuestMemory& memory) const;
};

struct ConsumerInfo final : ObjectPayload {
    Address data{};                       // +1 CFMutableData
    std::string path;                     // file consumer (guest path), written when released
    std::vector<std::uint8_t> file_bytes;
    Address info{}, put{}, release{};     // CGDataConsumerCallbacks
};
struct Quartz;
// Appends bytes to a CGDataConsumer.
void consumer_write(Quartz& q, Address consumer, std::span<const std::uint8_t> bytes);

struct ImageInfo final : ObjectPayload {
    ImageDesc desc;
    Address space{};                  // +1 (0 for masks)
    Address provider{};               // +1 CGDataProviderRef
    Address mask_image{};             // +1 (CGImageCreateWithMask)
    bool should_interpolate{true};
    std::int32_t intent{};
    std::string type_identifier;      // UTI of the source file ("" when created in memory)
    std::mutex mutex;
    std::shared_ptr<const Pbgra> decoded;   // cache
};

struct GradientInfo final : ObjectPayload {
    struct Stop { double location{}; Rgba color; };
    std::vector<Stop> stops;          // sorted by location
    Address space{};                  // +1
    [[nodiscard]] Rgba at(double t) const;
};

struct FunctionInfo final : ObjectPayload {
    Address info{}, evaluate{}, release{};
    std::vector<double> domain, range;
    std::uint64_t inputs{}, outputs{};
};
struct ShadingInfo final : ObjectPayload {
    bool radial{};
    Point start{}, end{};
    double start_radius{}, end_radius{};
    bool extend_start{}, extend_end{};
    Address space{}, function{};      // +1
    std::shared_ptr<GradientInfo> sampled;   // the function sampled into stops
};

struct PatternInfo final : ObjectPayload {
    Address info{}, draw{}, release{};   // CGPatternCallbacks
    Rect bounds{};
    Affine matrix{};
    double x_step{}, y_step{};
    std::int32_t tiling{};
    bool colored{true};
};

struct PathInfo final : ObjectPayload {
    PathData data;
    bool mutable_path{};
};

// ---- fonts (CoreText over DirectWrite, src/alchemy/coretext) -------------------------
struct FontFace;   // DirectWrite font face + names + metrics (coretext/coretext_internal.hpp)
struct FontMetrics {
    double units_per_em{2048}, ascent{}, descent{}, leading{}, cap_height{}, x_height{};
    double underline_position{}, underline_thickness{}, slant{};
    Rect bounding_box{};              // design units
};
// A glyph run to draw: positions are in text space (y up) relative to the
// text position; `matrix` is the font matrix (CTFont matrix, identity usually).
struct GlyphRunDraw {
    std::shared_ptr<FontFace> face;
    double size{12};
    Affine matrix{};
    std::vector<std::uint16_t> glyphs;
    std::vector<Point> positions;
};
// Implemented by coretext/fonts.cpp.
[[nodiscard]] FontMetrics font_metrics(const FontFace& face);
// Glyph outlines in text space (y up), scaled to `size` and placed at `positions`.
[[nodiscard]] PathData glyph_outlines(const GlyphRunDraw& run);

// ---- the shared provider ---------------------------------------------------------------
struct Quartz;
// The process provider (created by install_quartz2d); nullptr before.
[[nodiscard]] Quartz* provider() noexcept;

// Host code of this layer runs inside a ProviderScope: exported functions,
// native methods, object destroy callbacks and the quartz2d.hpp contract
// functions. An ObjectRef whose last copy dies outside every scope (the
// object store destroying leaked payloads while the process tears down,
// when releasing would touch a half-destroyed store) leaks its object.
class ProviderScope {
public:
    ProviderScope() noexcept;
    ~ProviderScope();
    ProviderScope(const ProviderScope&) = delete;
    ProviderScope& operator=(const ProviderScope&) = delete;
    [[nodiscard]] static bool active() noexcept;
};
// `handler` run inside a ProviderScope.
[[nodiscard]] Handler scoped(Handler handler);

// A counted reference to a CF-type object held by host state that is copied
// around (graphics states): the last copy releases the object when it dies
// inside a ProviderScope (see above).
class ObjectRef {
public:
    ObjectRef() = default;
    [[nodiscard]] static ObjectRef retain(Address object);   // takes a new reference
    [[nodiscard]] static ObjectRef adopt(Address object);    // takes over a +1 reference
    [[nodiscard]] Address get() const { return holder_ ? holder_->object : 0; }
    explicit operator bool() const { return get() != 0; }
    void reset() { holder_.reset(); }

private:
    struct Holder {
        Address object{};
        Holder() = default;
        Holder(const Holder&) = delete;
        Holder& operator=(const Holder&) = delete;
        ~Holder();
    };
    std::shared_ptr<Holder> holder_;
};

struct Quartz : std::enable_shared_from_this<Quartz> {
    ProcessContext& process;
    ObjcRuntime& objc;
    CoreFoundation& cf;

    // Host classes of the CF types.
    Address context_class{}, path_class{}, mutable_path_class{}, color_class{}, color_space_class{}, image_class{};
    Address provider_class{}, gradient_class{}, pattern_class{}, function_class{}, shading_class{}, layer_class{};
    Address consumer_class{}, font_class{};   // font_class: CGFontRef

    Quartz(ProcessContext& context, ObjcRuntime& runtime, CoreFoundation& foundation)
        : process(context), objc(runtime), cf(foundation) {}

    [[nodiscard]] GuestMemory& memory() const { return process.calling_engine(); }
    // Registers (or finds) a host class for CF-type objects; exported under `library` when given.
    // The registration helpers take their caller's location, which the
    // registration audit reports as the owner.
    Address declare_class(std::string_view name, std::string_view superclass = "NSObject", std::string_view library = {});
    void function(std::string_view library, std::string symbol, Handler handler,
                  const std::source_location& where = std::source_location::current());
    // Data export holding a constant CFString.
    void string_constant(std::string_view library, std::string symbol, std::string_view value,
                         const std::source_location& where = std::source_location::current());
    void data_constant(std::string_view library, std::string symbol, std::span<const std::uint8_t> bytes,
                       const std::source_location& where = std::source_location::current());

    // CF-type objects: +1, destroyed by `destroy` (may run guest code) when released.
    Address create(Address cls, std::shared_ptr<ObjectPayload> payload, std::function<void(Address)> destroy = {});
    template <class T>
    [[nodiscard]] std::shared_ptr<T> find(Address object) const {
        return object ? objc.find_host_state<T>(object) : nullptr;
    }
    template <class T>
    [[nodiscard]] std::shared_ptr<T> require(Address object, std::string_view what) const {
        auto value = find<T>(object);
        if (!value) fail(std::string("object is not a ") + std::string(what));
        return value;
    }
    Address retain(Address object) { return object ? process.objects().retain(object) : 0; }
    void release(Address object) { if (object) process.objects().release(object); }
    Address autorelease(Address object);
    CallResult send(Address receiver, std::string_view selector, CallRegs registers = {});
    Address send_id(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {});
    [[nodiscard]] bool is_kind(Address object, Address cls);
    Address call_guest(Address function, std::initializer_list<Address> arguments);

    // Guest memory helpers.
    [[nodiscard]] std::vector<std::uint8_t> read(Address address, std::size_t size) const;
    void write(Address address, std::span<const std::uint8_t> bytes) const;
    [[nodiscard]] std::vector<std::uint16_t> read_units(Address address, std::size_t count) const;
    void write_units(Address address, std::span<const std::uint16_t> units) const;
    [[nodiscard]] double read_double(Address address) const { return std::bit_cast<double>(memory().read64(address)); }
    void write_double(Address address, double value) const { memory().write64(address, std::bit_cast<std::uint64_t>(value)); }
    [[nodiscard]] Affine read_affine(Address address) const;
    void write_affine(Address address, const Affine& value) const;
    [[nodiscard]] Rect read_rect(Address address) const;
    [[nodiscard]] Point read_point(Address address) const;
    // Zero-filled guest memory from the engine heap (a mapped region when the
    // engine has no heap); released with free().
    struct GuestBlock {
        Address address{};
        bool heap{};
    };
    [[nodiscard]] GuestBlock allocate(std::size_t size);
    void free(GuestBlock block);
    // A temporary guest buffer released at scope exit.
    struct Scratch {
        Quartz& owner;
        GuestBlock block;
        Scratch(Quartz& quartz, std::size_t size) : owner(quartz), block(quartz.allocate(size)) {}
        ~Scratch() { try { owner.free(block); } catch (...) {} }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;
        [[nodiscard]] Address address() const { return block.address; }
    };

    // CF helpers.
    [[nodiscard]] Address data_object(std::span<const std::uint8_t> bytes);   // +1 CFData
    [[nodiscard]] std::vector<std::uint8_t> data_bytes(Address data);          // CFData / NSData contents
    void append_data(Address mutable_data, std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::optional<double> number(Address object);
    [[nodiscard]] Address dictionary_get(Address dictionary, std::string_view key);
    [[nodiscard]] std::string string_text(Address object) { return object ? cf.utf8(object) : std::string(); }
    // Files named by guest paths (CFURL paths, C strings).
    [[nodiscard]] std::filesystem::path host_path(std::string_view guest) const;
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read_file(std::string_view guest) const;
    bool write_file(std::string_view guest, std::span<const std::uint8_t> bytes) const;

    // Color spaces and colors (color.cpp).
    Address make_color_space(ColorModel model, std::string name);   // +1
    [[nodiscard]] Address device_rgb();   // shared immortal DeviceRGB space
    [[nodiscard]] Address device_gray();
    [[nodiscard]] Address srgb();
    [[nodiscard]] std::shared_ptr<ColorSpaceInfo> space_info(Address space) { return require<ColorSpaceInfo>(space, "CGColorSpace"); }
    Address make_color(Address space, std::vector<double> components, Address pattern = 0);   // +1
    // Components (without alpha) of `space` to straight sRGB.
    [[nodiscard]] Rgba to_rgba(Address space, std::span<const double> components, double alpha);
    [[nodiscard]] Rgba color_rgba(Address color);

    // Images (image.cpp).
    Address make_provider(std::shared_ptr<const std::vector<std::uint8_t>> bytes);   // +1
    // +1 CGImage over host RGBA bytes in `layout`.
    Address make_image(const PixelLayout& layout, Address space, std::shared_ptr<const std::vector<std::uint8_t>> bytes,
                       std::string type_identifier = {});
    [[nodiscard]] std::shared_ptr<const Pbgra> image_pixels(Address image);

    // Paths (path objects).
    Address make_path(PathData data, bool mutable_path);   // +1

    // Pieces installed by each file.
    void install_geometry();
    void install_colors();
    void install_paths();
    void install_images();
    void install_contexts();
    void install_gradients();
    void install_coretext();
    void install_imageio();
    void install_appkit_drawing();
    void install_displays();   // displays.cpp

    std::recursive_mutex mutex;
    Address shared_device_rgb{}, shared_device_gray{}, shared_srgb{};
    std::map<std::string, Address> constant_colors;   // CGColorGetConstantColor (immortal)
};

// ---- registers (Apple ARM64) ----------------------------------------------------------
[[nodiscard]] inline double d(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }
inline void set_d(CpuState& s, unsigned index, double value) { s.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; }
[[nodiscard]] inline Point point_arg(const CpuState& s, unsigned first = 0) { return {d(s, first), d(s, first + 1)}; }
[[nodiscard]] inline Size size_arg(const CpuState& s, unsigned first = 0) { return {d(s, first), d(s, first + 1)}; }
[[nodiscard]] inline Rect rect_arg(const CpuState& s, unsigned first = 0) {
    return {{d(s, first), d(s, first + 1)}, {d(s, first + 2), d(s, first + 3)}};
}
inline void return_point(CpuState& s, Point p) { set_d(s, 0, p.x); set_d(s, 1, p.y); }
inline void return_size(CpuState& s, Size v) { set_d(s, 0, v.width); set_d(s, 1, v.height); }
inline void return_rect(CpuState& s, Rect r) {
    set_d(s, 0, r.origin.x); set_d(s, 1, r.origin.y); set_d(s, 2, r.size.width); set_d(s, 3, r.size.height);
}
inline void return_bool(CpuState& s, bool value) { s.x[0] = value ? 1 : 0; }
[[nodiscard]] inline bool bool_arg(const CpuState& s, unsigned index) { return (s.x[index] & 0xff) != 0; }
[[nodiscard]] inline std::int32_t int_arg(const CpuState& s, unsigned index) { return static_cast<std::int32_t>(s.x[index]); }
// Apple ARM64 named stack arguments are packed at their natural alignment.
struct StackReader {
    const CpuState& state;
    GuestMemory& memory;
    Address offset{};
    template <class T>
    T next() {
        const Address alignment = alignof(T) < 8 ? alignof(T) : 8;
        offset = (offset + alignment - 1) & ~(alignment - 1);
        std::array<std::uint8_t, sizeof(T)> bytes{};
        memory.read_bytes(state.sp + offset, bytes);
        offset += sizeof(T);
        return std::bit_cast<T>(bytes);
    }
};
inline void put_point(CallRegs& r, unsigned first, Point p) { r.set_d(first, p.x); r.set_d(first + 1, p.y); }
inline void put_rect(CallRegs& r, unsigned first, Rect v) {
    r.set_d(first, v.origin.x); r.set_d(first + 1, v.origin.y); r.set_d(first + 2, v.size.width); r.set_d(first + 3, v.size.height);
}
[[nodiscard]] inline Rect result_rect(const CallResult& r) { return {{r.d(0), r.d(1)}, {r.d(2), r.d(3)}}; }
[[nodiscard]] inline Size result_size(const CallResult& r) { return {r.d(0), r.d(1)}; }

} // namespace alchemy::gui::quartz2d
