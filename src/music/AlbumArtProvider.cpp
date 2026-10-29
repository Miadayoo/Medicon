#include "AlbumArtProvider.hpp"

#include <winrt/Windows.Graphics.Imaging.h>
#include <algorithm>

namespace medicon::music {

std::shared_ptr<const AlbumArt> AlbumArtProvider::load(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& thumbnail,
    std::stop_token stop) {
    if (!thumbnail) return {};
    try {
        using namespace winrt::Windows::Graphics::Imaging;
        const auto stream = await_result(thumbnail.OpenReadAsync(), stop);
        if (!stream) return {};
        // Reject unreasonable inputs before asking the OS decoder for pixels.
        if (stream.Size() > 16 * 1024 * 1024) return {};
        const auto decoder = await_result(BitmapDecoder::CreateAsync(stream), stop);
        if (!decoder) return {};
        const auto w = decoder.PixelWidth(), h = decoder.PixelHeight();
        if (!w || !h || w > 16384 || h > 16384) return {};
        const float scale = std::min(1.0F, 192.0F / static_cast<float>(std::max(w, h)));
        BitmapTransform transform;
        transform.ScaledWidth(std::max(1U, static_cast<unsigned>(static_cast<float>(w) * scale)));
        transform.ScaledHeight(std::max(1U, static_cast<unsigned>(static_cast<float>(h) * scale)));
        const auto data = await_result(decoder.GetPixelDataAsync(BitmapPixelFormat::Rgba8,
            BitmapAlphaMode::Straight, transform, ExifOrientationMode::IgnoreExifOrientation,
            ColorManagementMode::ColorManageToSRgb), stop);
        if (!data) return {};
        const auto pixels = data.DetachPixelData();
        auto result = std::make_shared<AlbumArt>();
        result->width = static_cast<int>(transform.ScaledWidth());
        result->height = static_cast<int>(transform.ScaledHeight());
        if (pixels.size() != static_cast<std::size_t>(result->width * result->height * 4)) return {};
        result->rgba.assign(pixels.begin(), pixels.end());
        if (cached_ && cached_->width == result->width && cached_->height == result->height && cached_->rgba == result->rgba)
            return cached_;
        cached_ = result;
        return result;
    } catch (...) {
        return {}; // Bad or missing art must never suppress metadata or notifications.
    }
}

} // namespace medicon::music
