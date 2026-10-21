#pragma once

// Private helpers of the Win32 gpu backend (src/alchemy/host/win32/gui/):
// COM on the calling thread, the shared WIC, Direct2D and DirectWrite
// factories, and geometry readback. Only files of this directory include it
// (and through it, Windows headers).

#include "alchemy/host/gpu.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <objbase.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <vector>

namespace alchemy::host::gpu::win32 {

using Microsoft::WRL::ComPtr;

// COM on the calling thread (multithreaded unless the thread already chose
// an apartment); false only when COM is unusable.
[[nodiscard]] bool ensure_com();
// The process's WIC imaging factory (created on first use); null when it
// cannot be created.
[[nodiscard]] IWICImagingFactory* wic_factory();
// The process's Direct2D and DirectWrite factories (d2d1.dll / dwrite.dll
// from System32, created on first use); null when unavailable.
[[nodiscard]] ID2D1Factory1* d2d_factory();
[[nodiscard]] IDWriteFactory3* dwrite_factory();

// Records simplified geometry (Direct2D outlines, DirectWrite glyph runs) as
// path elements, mapping each point through `transform`. A stack object.
class PathSink final : public ID2D1SimplifiedGeometrySink {
public:
    PathSink(std::vector<PathElement>& path, const Matrix& transform) : path_(path), transform_(transform) {}
    STDMETHOD_(ULONG, AddRef)() override { return ++references_; }
    STDMETHOD_(ULONG, Release)() override { return --references_; }
    STDMETHOD(QueryInterface)(REFIID id, void** object) override;
    STDMETHOD_(void, SetFillMode)(D2D1_FILL_MODE) override {}
    STDMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT) override {}
    STDMETHOD_(void, BeginFigure)(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) override;
    STDMETHOD_(void, AddLines)(const D2D1_POINT_2F* points, UINT32 count) override;
    STDMETHOD_(void, AddBeziers)(const D2D1_BEZIER_SEGMENT* segments, UINT32 count) override;
    STDMETHOD_(void, EndFigure)(D2D1_FIGURE_END end) override;
    STDMETHOD(Close)() override { return S_OK; }

private:
    [[nodiscard]] Point map(D2D1_POINT_2F p) const {
        return {transform_.a * p.x + transform_.c * p.y + transform_.tx, transform_.b * p.x + transform_.d * p.y + transform_.ty};
    }
    std::vector<PathElement>& path_;
    Matrix transform_;
    ULONG references_{1};
};

// The DirectWrite face behind a Font, and a Font for a DirectWrite face.
[[nodiscard]] IDWriteFontFace3* native(const Font& font) noexcept;
[[nodiscard]] Font wrap(ComPtr<IDWriteFontFace3> face);
[[nodiscard]] IDWriteFontCollection* native(const FontCollection& collection) noexcept;

} // namespace alchemy::host::gpu::win32
