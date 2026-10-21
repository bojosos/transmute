// Win32 host backend, area "gpu": image file decoding and encoding
// (include/alchemy/host/gpu.hpp) through the Windows Imaging Component (PNG,
// TIFF, JPEG, BMP, GIF, ICO). Apple-only formats (ICNS, HEIC without the
// codec pack) are not readable.
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <objbase.h>
#include <shlwapi.h>
#include <wincodec.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace alchemy::host::gpu {
namespace {
template <class T>
struct Com {
    T* value{};
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
// COM for the calling thread; the window server may already have initialized
// it (OLE drag and drop needs an STA), in which case it is left as it is.
struct Apartment {
    bool owned{};
    Apartment() {
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = SUCCEEDED(result);
    }
    ~Apartment() { if (owned) CoUninitialize(); }
};
bool factory(Com<IWICImagingFactory>& out) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(out.put())));
}
} // namespace

Result<Pixels> decode_image(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > (1ULL << 31)) return Error::invalid_argument;
    Apartment apartment;
    Com<IWICImagingFactory> wic;
    if (!factory(wic)) return Error::unsupported;
    Com<IStream> stream;
    stream.value = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
    if (!stream) return Error::no_memory;
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.value, nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()))) return Error::unsupported;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return Error::unsupported;
    Com<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.value, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return Error::unsupported;
    UINT width{}, height{};
    if (FAILED(converter->GetSize(&width, &height)) || !width || !height || width > 32768 || height > 32768) return Error::out_of_range;
    Pixels result;
    result.width = width;
    result.height = height;
    result.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.rgba.size()), result.rgba.data())))
        return Error::io;
    return result;
}

Result<std::vector<std::uint8_t>> encode_image(const Pixels& pixels, ImageFormat type) {
    if (!pixels.width || !pixels.height || pixels.rgba.size() != static_cast<std::size_t>(pixels.width) * pixels.height * 4)
        return Error::invalid_argument;
    GUID container{};
    switch (type) {
    case ImageFormat::tiff: container = GUID_ContainerFormatTiff; break;
    case ImageFormat::bmp: container = GUID_ContainerFormatBmp; break;
    case ImageFormat::gif: container = GUID_ContainerFormatGif; break;
    case ImageFormat::jpeg: container = GUID_ContainerFormatJpeg; break;
    case ImageFormat::png: container = GUID_ContainerFormatPng; break;
    }
    Apartment apartment;
    Com<IWICImagingFactory> wic;
    if (!factory(wic)) return Error::unsupported;
    Com<IStream> stream;
    stream.value = SHCreateMemStream(nullptr, 0);
    if (!stream) return Error::io;
    Com<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(container, nullptr, encoder.put())) || FAILED(encoder->Initialize(stream.value, WICBitmapEncoderNoCache)))
        return Error::io;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(frame.put(), options.put())) || FAILED(frame->Initialize(options.value)) ||
        FAILED(frame->SetSize(pixels.width, pixels.height)))
        return Error::io;
    // Formats without alpha (JPEG, BMP) get a converted copy.
    Com<IWICBitmap> source;
    if (FAILED(wic->CreateBitmapFromMemory(pixels.width, pixels.height, GUID_WICPixelFormat32bppRGBA, pixels.width * 4,
                                           static_cast<UINT>(pixels.rgba.size()), const_cast<BYTE*>(pixels.rgba.data()), source.put())))
        return Error::io;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&format))) return Error::io;
    Com<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source.value, format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)) ||
        FAILED(frame->WriteSource(converter.value, nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return Error::io;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return Error::io;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER start{};
    ULONG read{};
    if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)) ||
        FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) || read != bytes.size())
        return Error::io;
    return bytes;
}
} // namespace alchemy::host::gpu
