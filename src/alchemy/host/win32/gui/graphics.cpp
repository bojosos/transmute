// Win32 gpu backend: COM and the shared WIC factory (graphics.hpp).
#include "alchemy/host/win32/gui/graphics.hpp"

#include <mutex>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "uuid.lib")

namespace alchemy::host::gpu::win32 {

bool ensure_com() {
    thread_local bool initialized = false;
    thread_local bool usable = false;
    if (initialized) return usable;
    const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_FALSE: already initialized; RPC_E_CHANGED_MODE: the thread chose an
    // apartment already (the window server's OLE thread). Both leave COM usable.
    usable = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    initialized = true;
    return usable;
}

IWICImagingFactory* wic_factory() {
    if (!ensure_com()) return nullptr;
    static std::once_flag once;
    static ComPtr<IWICImagingFactory> factory;
    std::call_once(once, [] {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) factory.Reset();
    });
    return factory.Get();
}

namespace {
FARPROC system_function(const wchar_t* library, const char* name) {
    auto* module = GetModuleHandleW(library);
    if (!module) module = LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? GetProcAddress(module, name) : nullptr;
}
} // namespace

ID2D1Factory1* d2d_factory() {
    static std::once_flag once;
    static ComPtr<ID2D1Factory1> factory;
    std::call_once(once, [] {
        using Create = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
        const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(system_function(L"d2d1.dll", "D2D1CreateFactory")));
        D2D1_FACTORY_OPTIONS options{};
        if (!create || FAILED(create(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &options,
                                     reinterpret_cast<void**>(factory.GetAddressOf()))))
            factory.Reset();
    });
    return factory.Get();
}

IDWriteFactory3* dwrite_factory() {
    static std::once_flag once;
    static ComPtr<IDWriteFactory3> factory;
    std::call_once(once, [] {
        using Create = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
        const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(system_function(L"dwrite.dll", "DWriteCreateFactory")));
        if (!create || FAILED(create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
            factory.Reset();
    });
    return factory.Get();
}

STDMETHODIMP PathSink::QueryInterface(REFIID id, void** object) {
    if (id == __uuidof(ID2D1SimplifiedGeometrySink) || id == __uuidof(IUnknown)) {
        *object = static_cast<ID2D1SimplifiedGeometrySink*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}
STDMETHODIMP_(void) PathSink::BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) {
    path_.push_back({PathOp::move, {map(start)}});
}
STDMETHODIMP_(void) PathSink::AddLines(const D2D1_POINT_2F* points, UINT32 count) {
    for (UINT32 index = 0; index < count; ++index) path_.push_back({PathOp::line, {map(points[index])}});
}
STDMETHODIMP_(void) PathSink::AddBeziers(const D2D1_BEZIER_SEGMENT* segments, UINT32 count) {
    for (UINT32 index = 0; index < count; ++index)
        path_.push_back({PathOp::cubic, {map(segments[index].point1), map(segments[index].point2), map(segments[index].point3)}});
}
STDMETHODIMP_(void) PathSink::EndFigure(D2D1_FIGURE_END end) {
    if (end == D2D1_FIGURE_END_CLOSED) path_.push_back({PathOp::close, {}});
}

} // namespace alchemy::host::gpu::win32
