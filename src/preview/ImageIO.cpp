#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>

#include "ImageIO.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace medicon::preview {
namespace {

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

void check(HRESULT result, const char* what) {
    if (FAILED(result)) {
        char message[128];
        std::snprintf(message, sizeof message, "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

struct ComScope {
    bool owned = false;
    ComScope() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(result);
    }
    ~ComScope() { if (owned) CoUninitialize(); }
};

IWICImagingFactory* factory(Com<IWICImagingFactory>& holder) {
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                           reinterpret_cast<void**>(holder.put())), "Create WIC factory");
    return holder.p;
}

} // namespace

void save_png(const std::filesystem::path& path, const Image& image) {
    ComScope com;
    Com<IWICImagingFactory> wic;
    factory(wic);
    if (const auto parent = path.parent_path(); !parent.empty()) std::filesystem::create_directories(parent);
    Com<IWICStream> stream;
    check(wic->CreateStream(stream.put()), "CreateStream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open PNG for writing");
    Com<IWICBitmapEncoder> encoder;
    check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()), "Create PNG encoder");
    check(encoder->Initialize(stream.p, WICBitmapEncoderNoCache), "Initialize encoder");
    Com<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(frame.put(), nullptr), "Create frame");
    check(frame->Initialize(nullptr), "Initialize frame");
    check(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)), "SetSize");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "SetPixelFormat");
    std::vector<std::uint8_t> bgra(image.rgba.size());
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        bgra[i] = image.rgba[i + 2];
        bgra[i + 1] = image.rgba[i + 1];
        bgra[i + 2] = image.rgba[i];
        bgra[i + 3] = image.rgba[i + 3];
    }
    if (std::memcmp(&format, &GUID_WICPixelFormat32bppBGRA, sizeof format) != 0) throw std::runtime_error("PNG encoder rejected BGRA");
    check(frame->WritePixels(static_cast<UINT>(image.height), static_cast<UINT>(image.width) * 4,
                             static_cast<UINT>(bgra.size()), bgra.data()), "WritePixels");
    check(frame->Commit(), "Commit frame");
    check(encoder->Commit(), "Commit encoder");
}

Image load_image(const std::filesystem::path& path) {
    ComScope com;
    Com<IWICImagingFactory> wic;
    factory(wic);
    Com<IWICBitmapDecoder> decoder;
    check(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()),
          "Open image");
    Com<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, frame.put()), "GetFrame");
    Com<IWICFormatConverter> converter;
    check(wic->CreateFormatConverter(converter.put()), "CreateFormatConverter");
    check(converter->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom), "Convert to RGBA");
    UINT width = 0;
    UINT height = 0;
    check(converter->GetSize(&width, &height), "GetSize");
    Image image;
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data()), "CopyPixels");
    return image;
}

} // namespace medicon::preview
