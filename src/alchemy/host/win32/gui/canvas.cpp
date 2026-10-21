// Win32 host backend, area "gpu": 2D drawing for the Quartz 2D pipeline
// (include/alchemy/host/gpu.hpp). A Surface is a software Direct2D render
// target over a WIC bitmap in premultiplied BGRA: between draws its pixels
// are plain memory (Direct2D reads the bitmap at BeginDraw and writes it back
// at EndDraw). Geometries are Direct2D path geometries.
#include "alchemy/host/win32/gui/graphics.hpp"

#include <d2d1_1helper.h>

#include <algorithm>

namespace alchemy::host::gpu {

struct Geometry::Impl {
    win32::ComPtr<ID2D1PathGeometry> geometry;
};
struct Surface::Impl {
    std::uint32_t width{}, height{};
    win32::ComPtr<IWICBitmap> bitmap;
    win32::ComPtr<ID2D1RenderTarget> target;
    win32::ComPtr<ID2D1DeviceContext> context;
    bool drawing{};
};

namespace {

using win32::ComPtr;
// Results holding a ComPtr are read with operator*: ComPtr overloads unary
// '&', and taking its address through Result's operator-> would release it.

[[nodiscard]] D2D1_POINT_2F to_d2d(Point p) { return {static_cast<float>(p.x), static_cast<float>(p.y)}; }
[[nodiscard]] D2D1_MATRIX_3X2_F to_d2d(const Matrix& m) {
    return {static_cast<float>(m.a), static_cast<float>(m.b), static_cast<float>(m.c), static_cast<float>(m.d),
            static_cast<float>(m.tx), static_cast<float>(m.ty)};
}
[[nodiscard]] D2D1_COLOR_F to_d2d(const Color& c) {
    return {static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b), static_cast<float>(c.a)};
}
[[nodiscard]] D2D1_RECT_F to_d2d(const Box& b) {
    return {static_cast<float>(b.left), static_cast<float>(b.top), static_cast<float>(b.right), static_cast<float>(b.bottom)};
}

Status status(HRESULT result) { return SUCCEEDED(result) ? Status{} : Status{host::win32::error(static_cast<DWORD>(result))}; }

D2D1_CAP_STYLE cap(StrokeStyle::Cap value) {
    return value == StrokeStyle::Cap::round ? D2D1_CAP_STYLE_ROUND : value == StrokeStyle::Cap::square ? D2D1_CAP_STYLE_SQUARE : D2D1_CAP_STYLE_FLAT;
}
D2D1_LINE_JOIN join(StrokeStyle::Join value) {
    return value == StrokeStyle::Join::round ? D2D1_LINE_JOIN_ROUND : value == StrokeStyle::Join::bevel ? D2D1_LINE_JOIN_BEVEL
                                                                                                         : D2D1_LINE_JOIN_MITER_OR_BEVEL;
}
Result<ComPtr<ID2D1StrokeStyle>> stroke_style(const StrokeStyle& style) {
    auto* factory = win32::d2d_factory();
    if (!factory) return Error::unsupported;
    D2D1_STROKE_STYLE_PROPERTIES properties{cap(style.cap), cap(style.cap), cap(style.cap), join(style.join),
                                            static_cast<float>(style.miter_limit), D2D1_DASH_STYLE_SOLID, 0};
    std::vector<float> pattern;
    for (const auto value : style.dashes) pattern.push_back(static_cast<float>(value));
    if (!pattern.empty()) {
        properties.dashStyle = D2D1_DASH_STYLE_CUSTOM;
        properties.dashOffset = static_cast<float>(style.dash_offset);
    }
    ComPtr<ID2D1StrokeStyle> result;
    if (FAILED(factory->CreateStrokeStyle(properties, pattern.empty() ? nullptr : pattern.data(), static_cast<UINT32>(pattern.size()), &result)))
        return Error::invalid_argument;
    return result;
}

Result<ComPtr<ID2D1Bitmap>> make_bitmap(ID2D1RenderTarget& target, const BgraView& pixels) {
    ComPtr<ID2D1Bitmap> bitmap;
    const auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(target.CreateBitmap(D2D1::SizeU(pixels.width, pixels.height), pixels.pixels, pixels.width * 4, properties, &bitmap)))
        return Error::no_memory;
    return bitmap;
}

Result<ComPtr<ID2D1Brush>> make_brush(ID2D1RenderTarget& target, const Brush& brush) {
    switch (brush.kind) {
    case Brush::Kind::solid: {
        ComPtr<ID2D1SolidColorBrush> solid;
        if (FAILED(target.CreateSolidColorBrush(to_d2d(brush.color), &solid))) return Error::no_memory;
        return ComPtr<ID2D1Brush>(solid);
    }
    case Brush::Kind::tiles: {
        auto bitmap = make_bitmap(target, brush.tiles);
        if (!bitmap) return bitmap.error();
        ComPtr<ID2D1BitmapBrush> tiles;
        if (FAILED(target.CreateBitmapBrush((*bitmap).Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                             D2D1::BrushProperties(static_cast<float>(brush.opacity), to_d2d(brush.tiles_transform)), &tiles)))
            return Error::no_memory;
        return ComPtr<ID2D1Brush>(tiles);
    }
    case Brush::Kind::linear_gradient: {
        std::vector<D2D1_GRADIENT_STOP> stops;
        for (const auto& stop : brush.stops) stops.push_back({static_cast<FLOAT>(stop.position), to_d2d(stop.color)});
        ComPtr<ID2D1GradientStopCollection> collection;
        if (FAILED(target.CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                                                       &collection)))
            return Error::invalid_argument;
        ComPtr<ID2D1LinearGradientBrush> linear;
        if (FAILED(target.CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(to_d2d(brush.start), to_d2d(brush.end)),
                                                    D2D1::BrushProperties(static_cast<FLOAT>(brush.opacity)), collection.Get(), &linear)))
            return Error::no_memory;
        return ComPtr<ID2D1Brush>(linear);
    }
    }
    return Error::invalid_argument;
}

} // namespace

// ---- Geometry ----------------------------------------------------------------------------------
Result<Geometry> Geometry::path(std::span<const PathElement> elements, bool even_odd, bool filled) {
    auto* factory = win32::d2d_factory();
    if (!factory) return Error::unsupported;
    auto impl = std::make_shared<Impl>();
    if (FAILED(factory->CreatePathGeometry(&impl->geometry))) return Error::no_memory;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(impl->geometry->Open(&sink))) return Error::no_memory;
    sink->SetFillMode(even_odd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
    bool open = false;
    Point start{}, last{};
    const auto begin_figure = [&](Point p) {
        sink->BeginFigure(to_d2d(p), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        open = true;
        start = last = p;
    };
    for (const auto& element : elements) {
        switch (element.op) {
        case PathOp::move:
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            open = false;
            start = last = element.points[0];
            begin_figure(element.points[0]);
            break;
        case PathOp::line:
            if (!open) begin_figure(last);
            sink->AddLine(to_d2d(element.points[0]));
            last = element.points[0];
            break;
        case PathOp::quad: {
            if (!open) begin_figure(last);
            const D2D1_QUADRATIC_BEZIER_SEGMENT segment{to_d2d(element.points[0]), to_d2d(element.points[1])};
            sink->AddQuadraticBezier(segment);
            last = element.points[1];
            break;
        }
        case PathOp::cubic: {
            if (!open) begin_figure(last);
            const D2D1_BEZIER_SEGMENT segment{to_d2d(element.points[0]), to_d2d(element.points[1]), to_d2d(element.points[2])};
            sink->AddBezier(segment);
            last = element.points[2];
            break;
        }
        case PathOp::close:
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            last = start;
            break;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return Error::invalid_argument;
    return Geometry(std::move(impl));
}

Result<std::vector<PathElement>> Geometry::widened(double width, const StrokeStyle& style, const Matrix& transform) const {
    if (!impl_) return Error::bad_handle;
    auto* factory = win32::d2d_factory();
    auto stroke = stroke_style(style);
    if (!stroke) return stroke.error();
    ComPtr<ID2D1PathGeometry> widened;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&widened)) || FAILED(widened->Open(&sink))) return Error::no_memory;
    if (FAILED(impl_->geometry->Widen(static_cast<float>(width), (*stroke).Get(), nullptr, 0.05F, sink.Get())) || FAILED(sink->Close()))
        return Error::invalid_argument;
    std::vector<PathElement> path;
    win32::PathSink reader(path, transform);
    if (FAILED(widened->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES, nullptr, 0.05F, &reader))) return Error::io;
    return path;
}

Result<bool> Geometry::stroke_contains(Point point, double width) const {
    if (!impl_) return Error::bad_handle;
    BOOL hit{};
    if (FAILED(impl_->geometry->StrokeContainsPoint(to_d2d(point), static_cast<float>(width), nullptr, nullptr, 0.05F, &hit)))
        return Error::invalid_argument;
    return hit != FALSE;
}

// ---- Surface -----------------------------------------------------------------------------------
Result<std::unique_ptr<Surface>> Surface::create(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return Error::invalid_argument;
    auto* wic = win32::wic_factory();
    auto* d2d = win32::d2d_factory();
    if (!wic || !d2d) return Error::unsupported;
    auto impl = std::make_unique<Impl>();
    impl->width = width;
    impl->height = height;
    if (const auto created = wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &impl->bitmap); FAILED(created))
        return status(created).error();
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
    if (const auto created = d2d->CreateWicBitmapRenderTarget(impl->bitmap.Get(), properties, &impl->target); FAILED(created))
        return status(created).error();
    static_cast<void>(impl->target.As(&impl->context));
    impl->target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return std::make_unique<Surface>(std::move(impl));
}

Surface::Surface(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Surface::~Surface() = default;
std::uint32_t Surface::width() const noexcept { return impl_->width; }
std::uint32_t Surface::height() const noexcept { return impl_->height; }
bool Surface::blend_modes() const noexcept { return impl_->context != nullptr; }

Status Surface::begin() {
    if (impl_->drawing) return Error::busy;
    impl_->target->BeginDraw();
    impl_->target->SetTransform(D2D1::Matrix3x2F::Identity());
    impl_->drawing = true;
    return {};
}
Status Surface::end() {
    if (!impl_->drawing) return {};
    impl_->drawing = false;
    D2D1_TAG first{}, second{};
    return status(impl_->target->EndDraw(&first, &second));
}

Result<Surface::Lock> Surface::lock(const PixelRect& rect, bool write) {
    if (impl_->drawing) return Error::busy;
    ComPtr<IWICBitmapLock> locked;
    const WICRect area{rect.x0, rect.y0, static_cast<INT>(rect.width()), static_cast<INT>(rect.height())};
    if (const auto result = impl_->bitmap->Lock(&area, write ? WICBitmapLockWrite | WICBitmapLockRead : WICBitmapLockRead, &locked); FAILED(result))
        return status(result).error();
    UINT size{}, stride{};
    BYTE* data{};
    if (FAILED(locked->GetDataPointer(&size, &data)) || FAILED(locked->GetStride(&stride))) return Error::io;
    Lock result;
    result.data = data;
    result.stride = stride;
    result.hold = std::shared_ptr<void>(locked.Detach(), [](void* lock) { static_cast<IWICBitmapLock*>(lock)->Release(); });
    return result;
}

void Surface::set_transform(const Matrix& transform) { impl_->target->SetTransform(to_d2d(transform)); }
void Surface::set_antialias(bool antialias) {
    impl_->target->SetAntialiasMode(antialias ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
}
void Surface::set_text_antialias(bool grayscale) {
    impl_->target->SetTextAntialiasMode(grayscale ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
}
void Surface::set_blend(Blend blend) {
    if (!impl_->context) return;
    impl_->context->SetPrimitiveBlend(blend == Blend::copy  ? D2D1_PRIMITIVE_BLEND_COPY
                                      : blend == Blend::add ? D2D1_PRIMITIVE_BLEND_ADD
                                                            : D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}
void Surface::push_axis_clip(const Box& box, bool antialias) {
    impl_->target->PushAxisAlignedClip(to_d2d(box), antialias ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
}
void Surface::pop_axis_clip() { impl_->target->PopAxisAlignedClip(); }
Status Surface::push_clip(const Geometry& geometry, bool antialias) {
    if (!geometry) return Error::bad_handle;
    ComPtr<ID2D1Layer> layer;
    if (const auto created = impl_->target->CreateLayer(nullptr, &layer); FAILED(created)) return status(created);
    impl_->target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.impl()->geometry.Get(),
                                                   antialias ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED),
                             layer.Get());
    return {};
}
void Surface::pop_clip() { impl_->target->PopLayer(); }

Status Surface::fill_rect(const Box& box, const Brush& brush) {
    auto made = make_brush(*impl_->target.Get(), brush);
    if (!made) return made.error();
    impl_->target->FillRectangle(to_d2d(box), (*made).Get());
    return {};
}
Status Surface::fill(const Geometry& geometry, const Brush& brush) {
    if (!geometry) return Error::bad_handle;
    auto made = make_brush(*impl_->target.Get(), brush);
    if (!made) return made.error();
    impl_->target->FillGeometry(geometry.impl()->geometry.Get(), (*made).Get());
    return {};
}
Status Surface::stroke(const Geometry& geometry, const Brush& brush, double width, const StrokeStyle& style) {
    if (!geometry) return Error::bad_handle;
    auto made = make_brush(*impl_->target.Get(), brush);
    if (!made) return made.error();
    auto stroke = stroke_style(style);
    if (!stroke) return stroke.error();
    impl_->target->DrawGeometry(geometry.impl()->geometry.Get(), (*made).Get(), static_cast<float>(width), (*stroke).Get());
    return {};
}
Status Surface::draw_bitmap(const BgraView& pixels, const Box& destination, double opacity, Interpolation interpolation) {
    auto bitmap = make_bitmap(*impl_->target.Get(), pixels);
    if (!bitmap) return bitmap.error();
    const auto box = to_d2d(destination);
    if (interpolation == Interpolation::high_quality && impl_->context) {
        impl_->context->DrawBitmap((*bitmap).Get(), &box, static_cast<float>(opacity), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        return {};
    }
    impl_->target->DrawBitmap((*bitmap).Get(), box, static_cast<float>(opacity),
                              interpolation == Interpolation::nearest ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                                      : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return {};
}
Status Surface::draw_glyphs(const Font& font, double size, std::span<const std::uint16_t> glyphs, std::span<const Point> offsets,
                            const Brush& brush) {
    auto* face = win32::native(font);
    if (!face) return Error::bad_handle;
    auto made = make_brush(*impl_->target.Get(), brush);
    if (!made) return made.error();
    std::vector<FLOAT> advances(glyphs.size(), 0.0F);
    std::vector<DWRITE_GLYPH_OFFSET> placed(glyphs.size());
    for (std::size_t index = 0; index < placed.size(); ++index) {
        const auto p = index < offsets.size() ? offsets[index] : Point{};
        placed[index] = {static_cast<FLOAT>(p.x), static_cast<FLOAT>(p.y)};
    }
    DWRITE_GLYPH_RUN run{};
    run.fontFace = face;
    run.fontEmSize = static_cast<FLOAT>(size);
    run.glyphCount = static_cast<UINT32>(glyphs.size());
    run.glyphIndices = glyphs.data();
    run.glyphAdvances = advances.data();
    run.glyphOffsets = placed.data();
    impl_->target->DrawGlyphRun(D2D1::Point2F(0, 0), &run, (*made).Get(), DWRITE_MEASURING_MODE_NATURAL);
    return {};
}

} // namespace alchemy::host::gpu
