#include <medicon/gui/SocialBackground.hpp>

#include <medicon/gui/Draw.hpp>
#include <medicon/gui/Log.hpp>
#include <medicon/gui/Textures.hpp>
#include <medicon/gui/Theme.hpp>
#include <medicon_embedded_social.hpp>

#include <Windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace medicon::gui {
namespace {

template <class T> struct Com {
    T* pointer = nullptr;
    ~Com() { if (pointer) pointer->Release(); }
    T** put() { return &pointer; }
    T* operator->() const { return pointer; }
};

struct ComScope {
    // The worker thread may already be in another apartment; balance only our own initialization.
    bool owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ~ComScope() { if (owned) CoUninitialize(); }
};

// Texture pixels per standard deviation of the blur. The blurred picture has no
// detail finer than this, so a smaller texture stretched by the GPU looks the
// same, and the worker's blur stays cheap at any window size.
constexpr float texture_sigma = 2.5F;

bool same_size(ImVec2 a, ImVec2 b) { return a.x == b.x && a.y == b.y; }

// This render target is private to the background worker. It never references
// the game's swap chain or the renderer's resources: only the finished RGBA
// bytes cross back to the presentation thread via SocialBackground's future.
bool d2d_blur(SocialBackground::Image& image, float sigma) {
    // The SDK declares this effect CLSID but some MinGW distributions do not
    // provide its import symbol; keep the SDK value local to this translation unit.
    constexpr GUID gaussian_blur_id{0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};
    ComScope com;
    const UINT width = static_cast<UINT>(image.width), height = static_cast<UINT>(image.height);
    if (!width || !height || !(sigma > 0.0F)) return false;
    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                reinterpret_cast<void**>(wic.put())))) return false;
    Com<IWICBitmap> destination;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                 destination.put()))) return false;
    Com<ID2D1Factory> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put()))) return false;
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
    Com<ID2D1RenderTarget> target;
    if (FAILED(factory->CreateWicBitmapRenderTarget(destination.pointer, properties, target.put()))) return false;
    Com<ID2D1DeviceContext> context;
    if (FAILED(target->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(context.put())))) return false;

    // D2D's BGRA premultiplied pixels differ from TextureHost's straight RGBA.
    std::vector<std::uint8_t> bgra(image.rgba.size());
    for (std::size_t i = 0; i < bgra.size(); i += 4) {
        const unsigned alpha = image.rgba[i + 3];
        bgra[i] = static_cast<std::uint8_t>((image.rgba[i + 2] * alpha + 127) / 255);
        bgra[i + 1] = static_cast<std::uint8_t>((image.rgba[i + 1] * alpha + 127) / 255);
        bgra[i + 2] = static_cast<std::uint8_t>((image.rgba[i] * alpha + 127) / 255);
        bgra[i + 3] = static_cast<std::uint8_t>(alpha);
    }
    const auto bitmap_properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0F, 96.0F);
    Com<ID2D1Bitmap> source;
    if (FAILED(target->CreateBitmap(D2D1::SizeU(width, height), bgra.data(), width * 4, bitmap_properties, source.put())))
        return false;
    Com<ID2D1Effect> effect;
    if (FAILED(context->CreateEffect(gaussian_blur_id, effect.put()))) return false;
    effect->SetInput(0, source.pointer);
    if (FAILED(effect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma)) ||
        FAILED(effect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD))) return false;
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0, 0.0F));
    context->DrawImage(effect.pointer);
    if (FAILED(context->EndDraw())) return false;
    Com<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(destination.pointer, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    std::vector<std::uint8_t> result(image.rgba.size());
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(result.size()), result.data()))) return false;
    image.rgba.swap(result);
    return true;
}

} // namespace

SocialBackground::SocialBackground()
    : SocialBackground(std::span<const std::uint8_t>(embedded_social::back, sizeof embedded_social::back)) {}
SocialBackground::SocialBackground(std::span<const std::uint8_t> png) : png_(png) {}
SocialBackground::~SocialBackground() = default;

std::shared_ptr<const SocialBackground::Image> SocialBackground::decode(std::span<const std::uint8_t> png) {
    ComScope com;
    Com<IWICImagingFactory> factory;
    if (png.empty() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put()))))
        return nullptr;
    // WIC only reads from the stream; the bytes stay owned by the caller.
    Com<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))))
        return nullptr;
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.pointer, nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())))
        return nullptr;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> converter;
    if (FAILED(decoder->GetFrame(0, frame.put())) || FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.pointer, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return nullptr;
    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > 8192 || height > 8192)
        return nullptr;
    auto image = std::make_shared<Image>();
    image->width = static_cast<int>(width);
    image->height = static_cast<int>(height);
    image->rgba.resize(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image->rgba.size()), image->rgba.data())))
        return nullptr;
    return image;
}

void SocialBackground::gaussian_blur(Image& image, float sigma) {
    const int width = image.width, height = image.height;
    if (!(sigma > 0.0F) || width <= 0 || height <= 0) return;
    const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0F)));
    std::vector<float> kernel(static_cast<std::size_t>(radius) * 2 + 1);
    float sum = 0.0F;
    for (int i = -radius; i <= radius; ++i) {
        const float weight = std::exp(-static_cast<float>(i * i) / (2.0F * sigma * sigma));
        kernel[static_cast<std::size_t>(i + radius)] = weight;
        sum += weight;
    }
    for (auto& weight : kernel) weight /= sum;
    const auto at = [](int x, int y, int stride) { return (static_cast<std::size_t>(y) * stride + x) * 4; };
    std::vector<float> rows(image.rgba.size());
    // Separable: horizontal into floats, then vertical back into bytes. Samples
    // past an edge repeat the edge pixel, so the border keeps the picture's colour
    // instead of fading to transparent.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float channels[4]{};
            for (int i = -radius; i <= radius; ++i) {
                const auto source = at(std::clamp(x + i, 0, width - 1), y, width);
                const float weight = kernel[static_cast<std::size_t>(i + radius)];
                for (int c = 0; c < 4; ++c) channels[c] += weight * image.rgba[source + c];
            }
            const auto target = at(x, y, width);
            for (int c = 0; c < 4; ++c) rows[target + c] = channels[c];
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float channels[4]{};
            for (int i = -radius; i <= radius; ++i) {
                const auto source = at(x, std::clamp(y + i, 0, height - 1), width);
                const float weight = kernel[static_cast<std::size_t>(i + radius)];
                for (int c = 0; c < 4; ++c) channels[c] += weight * rows[source + c];
            }
            const auto target = at(x, y, width);
            for (int c = 0; c < 4; ++c)
                image.rgba[target + c] = static_cast<std::uint8_t>(std::clamp(std::lround(channels[c]), 0L, 255L));
        }
    }
}

std::unique_ptr<SocialBackground::Image> SocialBackground::render(std::shared_ptr<const Image> source, ImVec2 size,
                                                                  float sigma) {
    const float factor = std::max(1.0F, std::round(sigma / texture_sigma));
    auto image = std::make_unique<Image>();
    image->width = std::max(1, static_cast<int>(std::ceil(size.x / factor)));
    image->height = std::max(1, static_cast<int>(std::ceil(size.y / factor)));
    image->rgba.resize(static_cast<std::size_t>(image->width) * image->height * 4);
    // Cover the target keeping the aspect ratio; the overflow is cropped evenly.
    const float scale = std::max(static_cast<float>(image->width) / static_cast<float>(source->width),
                                 static_cast<float>(image->height) / static_cast<float>(source->height));
    const float offset_x = (static_cast<float>(source->width) * scale - static_cast<float>(image->width)) * 0.5F;
    const float offset_y = (static_cast<float>(source->height) * scale - static_cast<float>(image->height)) * 0.5F;
    const auto pixel = [&](int x, int y, int c) {
        x = std::clamp(x, 0, source->width - 1);
        y = std::clamp(y, 0, source->height - 1);
        return static_cast<float>(source->rgba[(static_cast<std::size_t>(y) * source->width + x) * 4 + c]);
    };
    for (int y = 0; y < image->height; ++y) {
        const float sy = (static_cast<float>(y) + 0.5F + offset_y) / scale - 0.5F;
        const int y0 = static_cast<int>(std::floor(sy));
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < image->width; ++x) {
            const float sx = (static_cast<float>(x) + 0.5F + offset_x) / scale - 0.5F;
            const int x0 = static_cast<int>(std::floor(sx));
            const float fx = sx - static_cast<float>(x0);
            for (int c = 0; c < 4; ++c) {
                const float top = pixel(x0, y0, c) * (1.0F - fx) + pixel(x0 + 1, y0, c) * fx;
                const float bottom = pixel(x0, y0 + 1, c) * (1.0F - fx) + pixel(x0 + 1, y0 + 1, c) * fx;
                image->rgba[(static_cast<std::size_t>(y) * image->width + x) * 4 + c] =
                    static_cast<std::uint8_t>(std::lround(top * (1.0F - fy) + bottom * fy));
            }
        }
    }
    // The output is reduced by `factor`, so scaling the cached texture back to
    // `size` yields the requested standard deviation.
    if (!d2d_blur(*image, sigma / factor)) gaussian_blur(*image, sigma / factor);
    return image;
}

void SocialBackground::forget_texture() noexcept {
    backdrop_.texture = 0;
    panel_.texture = 0;
    host_ = nullptr;
}

bool SocialBackground::ready(ImVec2 size) const noexcept {
    return backdrop_.texture && same_size(backdrop_.blurred_size, size);
}

bool SocialBackground::panel_ready(ImVec2 size) const noexcept {
    return panel_.texture && same_size(panel_.blurred_size, size);
}

bool SocialBackground::source_ready() {
    if (source_) return true;
    if (failed_) return false;
    if (!decoding_.valid()) decoding_ = std::async(std::launch::async, &SocialBackground::decode, png_);
    if (decoding_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    source_ = decoding_.get();
    if (!source_) {
        failed_ = true;
        log_line("Social background: the bundled back.png could not be decoded; showing the gradient alone.");
        return false;
    }
    return true;
}

void SocialBackground::update_layer(Layer& layer, ImVec2 size, float sigma, TextureHost* host) {
    if (!(size.x >= 1.0F) || !(size.y >= 1.0F) || !source_ready()) return;
    if (layer.rendering.valid() && layer.rendering.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (auto image = layer.rendering.get()) {
            layer.blurred = std::move(image);
            layer.blurred_size = layer.rendering_size;
            if (layer.texture && host_) host_->release(layer.texture);
            layer.texture = 0;
        }
    }
    if (host && layer.blurred && !layer.texture) {
        layer.texture = host->create(layer.blurred->rgba.data(), layer.blurred->width, layer.blurred->height);
        if (layer.texture) host_ = host;
    }
    // One job at a time: a live resize renders the latest size once the current job ends.
    if (!layer.rendering.valid() && (!layer.blurred || !same_size(layer.blurred_size, size))) {
        layer.rendering_size = size;
        layer.rendering = std::async(std::launch::async, &SocialBackground::render, source_, size, sigma);
    }
}

void SocialBackground::update(ImVec2 size, TextureHost* host) {
    // Spec 4.6: sigma = 8 x scale, scale = window height / 1080.
    update_layer(backdrop_, size, theme::social::metrics::background_blur * size.y / theme::social::metrics::reference_height,
                 host);
}

void SocialBackground::update_panel(ImVec2 size, float sigma, TextureHost* host) { update_layer(panel_, size, sigma, host); }

void SocialBackground::draw(ImDrawList* list, ImVec2 size, float alpha) const {
    if (!list || !(alpha > 0.0F)) return;
    // Laid over the Main Menu gradient, which stays visible through it. During a
    // resize the previous picture is stretched until the new one is ready: a
    // blurred image looks the same scaled. Until the first one exists the
    // gradient alone is shown.
    if (backdrop_.texture) {
        list->AddImage(backdrop_.texture, {0.0F, 0.0F}, size, {0.0F, 0.0F}, {1.0F, 1.0F},
                       to_u32(theme::white, alpha * theme::social::background_picture));
    }
    list->AddRectFilled({0.0F, 0.0F}, size, to_u32(theme::social::background_veil, alpha));
}

void SocialBackground::draw_panel(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, ImDrawFlags corners,
                                  float alpha) const {
    if (!list || !panel_.texture || !(alpha > 0.0F)) return;
    // Stretched like the backdrop while a resize is being rendered.
    list->AddImageRounded(panel_.texture, min, max, {0.0F, 0.0F}, {1.0F, 1.0F},
                          to_u32(theme::white, alpha * theme::social::panel_picture), rounding, corners);
    list->AddRectFilled(min, max, to_u32(theme::social::panel_picture_veil, alpha), rounding, corners);
}

} // namespace medicon::gui
