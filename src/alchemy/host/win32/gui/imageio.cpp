// Win32 host backend, area "gpu": image files with frames and metadata for
// ImageIO (include/alchemy/host/gpu.hpp), through the Windows Imaging
// Component: frame counts, frame headers (resolution, alpha, bits per
// component, GIF delays, EXIF orientation), RGBA pixels and multi-frame
// encoding.
#include "alchemy/host/gpu.hpp"
#include "alchemy/host/win32/gui/graphics.hpp"

#include <propidl.h>

#include <algorithm>
#include <cstring>
#include <optional>

namespace alchemy::host::gpu {
namespace {

using win32::ComPtr;

struct WicSource {
    std::vector<std::uint8_t> bytes;          // must outlive the decoder
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
};
std::optional<WicSource> open_wic(std::span<const std::uint8_t> bytes) {
    auto* factory = win32::wic_factory();
    if (!factory) return std::nullopt;
    WicSource source;
    source.bytes.assign(bytes.begin(), bytes.end());
    if (FAILED(factory->CreateStream(&source.stream))) return std::nullopt;
    if (FAILED(source.stream->InitializeFromMemory(source.bytes.data(), static_cast<DWORD>(source.bytes.size())))) return std::nullopt;
    if (FAILED(factory->CreateDecoderFromStream(source.stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &source.decoder)))
        return std::nullopt;
    return source;
}

std::optional<std::uint32_t> metadata_number(IWICBitmapFrameDecode& frame, const wchar_t* query) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame.GetMetadataQueryReader(&reader))) return std::nullopt;
    PROPVARIANT value;
    PropVariantInit(&value);
    std::optional<std::uint32_t> result;
    if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
        if (value.vt == VT_UI2) result = value.uiVal;
        else if (value.vt == VT_UI4) result = value.ulVal;
        else if (value.vt == VT_UI1) result = value.bVal;
    }
    PropVariantClear(&value);
    return result;
}

} // namespace

Result<std::size_t> image_frame_count(std::span<const std::uint8_t> bytes) {
    const auto source = open_wic(bytes);
    if (!source) return Error::unsupported;
    UINT count{};
    if (FAILED(source->decoder->GetFrameCount(&count))) return Error::unsupported;
    return static_cast<std::size_t>(count);
}

Result<ImageFrame> image_frame(std::span<const std::uint8_t> bytes, std::size_t index, bool pixels) {
    auto source = open_wic(bytes);
    if (!source) return Error::unsupported;
    UINT count{};
    if (FAILED(source->decoder->GetFrameCount(&count)) || index >= count) return Error::out_of_range;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(source->decoder->GetFrame(static_cast<UINT>(index), &frame))) return Error::io;
    ImageFrame result;
    UINT width{}, height{};
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return Error::io;
    result.width = width;
    result.height = height;
    double dpi_x{}, dpi_y{};
    if (SUCCEEDED(frame->GetResolution(&dpi_x, &dpi_y)) && dpi_x > 0 && dpi_y > 0) { result.dpi_x = dpi_x; result.dpi_y = dpi_y; }
    WICPixelFormatGUID format{};
    if (SUCCEEDED(frame->GetPixelFormat(&format))) {
        ComPtr<IWICComponentInfo> info;
        ComPtr<IWICPixelFormatInfo2> pixel_info;
        if (SUCCEEDED(win32::wic_factory()->CreateComponentInfo(format, &info)) && SUCCEEDED(info.As(&pixel_info))) {
            BOOL transparency{};
            if (SUCCEEDED(pixel_info->SupportsTransparency(&transparency))) result.has_alpha = transparency != FALSE;
            UINT bits{}, channels{};
            if (SUCCEEDED(pixel_info->GetBitsPerPixel(&bits)) && SUCCEEDED(pixel_info->GetChannelCount(&channels)) && channels)
                result.bits_per_component = std::max(1U, bits / channels);
        }
    }
    GUID container{};
    static_cast<void>(source->decoder->GetContainerFormat(&container));
    if (container == GUID_ContainerFormatGif) {
        if (const auto delay = metadata_number(*frame.Get(), L"/grctlext/Delay")) result.delay = *delay / 100.0;
    } else if (container == GUID_ContainerFormatJpeg) {
        if (const auto orientation = metadata_number(*frame.Get(), L"/app1/ifd/{ushort=274}")) result.orientation = *orientation;
    } else if (container == GUID_ContainerFormatTiff) {
        if (const auto orientation = metadata_number(*frame.Get(), L"/ifd/{ushort=274}")) result.orientation = *orientation;
    }
    if (result.orientation < 1 || result.orientation > 8) result.orientation = 1;
    if (!pixels) return result;
    ComPtr<IWICBitmapSource> converted;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &converted))) return Error::unsupported;
    result.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converted->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.rgba.size()), result.rgba.data()))) return Error::io;
    if (!result.has_alpha)
        for (std::size_t at = 3; at < result.rgba.size(); at += 4) result.rgba[at] = 0xff;
    return result;
}

Result<std::vector<std::uint8_t>> encode_image_frames(ImageFormat type, std::span<const Pixels> frames, double quality) {
    if (frames.empty()) return Error::invalid_argument;
    GUID container{};
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    switch (type) {
    case ImageFormat::png: container = GUID_ContainerFormatPng; break;
    case ImageFormat::jpeg: container = GUID_ContainerFormatJpeg; format = GUID_WICPixelFormat24bppBGR; break;
    case ImageFormat::tiff: container = GUID_ContainerFormatTiff; break;
    case ImageFormat::bmp: container = GUID_ContainerFormatBmp; format = GUID_WICPixelFormat32bppBGRA; break;
    case ImageFormat::gif: container = GUID_ContainerFormatGif; format = GUID_WICPixelFormat8bppIndexed; break;
    }
    auto* factory = win32::wic_factory();
    if (!factory) return Error::unsupported;
    ComPtr<IStream> memory;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory))) return Error::no_memory;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(container, nullptr, &encoder)) || FAILED(encoder->Initialize(memory.Get(), WICBitmapEncoderNoCache)))
        return Error::unsupported;
    const auto count = (type == ImageFormat::tiff || type == ImageFormat::gif) ? frames.size() : 1;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& frame = frames[index];
        ComPtr<IWICBitmapFrameEncode> target;
        ComPtr<IPropertyBag2> properties;
        if (FAILED(encoder->CreateNewFrame(&target, &properties))) return Error::io;
        if (type == ImageFormat::jpeg && properties) {
            PROPBAG2 option{};
            wchar_t name[] = L"ImageQuality";
            option.pstrName = name;
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = static_cast<float>(std::clamp(quality, 0.0, 1.0));
            static_cast<void>(properties->Write(1, &option, &value));
        }
        if (FAILED(target->Initialize(properties.Get()))) return Error::io;
        if (FAILED(target->SetSize(frame.width, frame.height))) return Error::io;
        auto requested = format;
        if (FAILED(target->SetPixelFormat(&requested))) return Error::io;
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmapFromMemory(frame.width, frame.height, GUID_WICPixelFormat32bppRGBA, frame.width * 4,
                                                   static_cast<UINT>(frame.rgba.size()), const_cast<BYTE*>(frame.rgba.data()), &bitmap)))
            return Error::io;
        ComPtr<IWICBitmapSource> source = bitmap;
        if (requested != GUID_WICPixelFormat32bppRGBA) {
            if (requested == GUID_WICPixelFormat8bppIndexed) {
                ComPtr<IWICPalette> palette;
                ComPtr<IWICFormatConverter> converter;
                if (FAILED(factory->CreatePalette(&palette)) || FAILED(palette->InitializeFromBitmap(bitmap.Get(), 256, TRUE)) ||
                    FAILED(factory->CreateFormatConverter(&converter)) ||
                    FAILED(converter->Initialize(bitmap.Get(), requested, WICBitmapDitherTypeNone, palette.Get(), 0, WICBitmapPaletteTypeCustom)))
                    return Error::io;
                static_cast<void>(target->SetPalette(palette.Get()));
                source = converter;
            } else {
                ComPtr<IWICBitmapSource> converted;
                if (FAILED(WICConvertBitmapSource(requested, bitmap.Get(), &converted))) return Error::io;
                source = converted;
            }
        }
        if (FAILED(target->WriteSource(source.Get(), nullptr)) || FAILED(target->Commit())) return Error::io;
    }
    if (FAILED(encoder->Commit())) return Error::io;
    HGLOBAL global{};
    if (FAILED(GetHGlobalFromStream(memory.Get(), &global))) return Error::io;
    STATSTG stat{};
    if (FAILED(memory->Stat(&stat, STATFLAG_NONAME))) return Error::io;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(stat.cbSize.QuadPart));
    const auto* pointer = static_cast<const std::uint8_t*>(GlobalLock(global));
    if (!pointer) return Error::io;
    std::memcpy(out.data(), pointer, out.size());
    GlobalUnlock(global);
    return out;
}

} // namespace alchemy::host::gpu
